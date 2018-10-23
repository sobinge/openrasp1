/*
 * Copyright 2017-2018 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <memory>

#include "openrasp_log.h"
#include "openrasp_shared_alloc.h"
#include "openrasp_agent_manager.h"
#include <string>
#include <vector>
#include <iostream>
#include "openrasp_ini.h"
#include "utils/digest.h"
#include "openrasp_utils.h"

extern "C"
{
#include "ext/standard/php_smart_str.h"
#include "ext/json/php_json.h"
#include "php_streams.h"
#include "php_main.h"
}

namespace openrasp
{
std::unique_ptr<OpenraspAgentManager> oam = nullptr;

std::vector<std::unique_ptr<BaseAgent>> agents;

static void super_signal_handler(int signal_no)
{
	exit(0);
}

static void super_install_signal_handler()
{
	struct sigaction sa_usr = {0};
	sa_usr.sa_flags = 0;
	sa_usr.sa_handler = super_signal_handler;
	sigaction(SIGTERM, &sa_usr, NULL);
}

static void supervisor_sigchld_handler(int signal_no)
{
	pid_t p;
	int status;
	while ((p = waitpid(-1, &status, WNOHANG)) > 0)
	{
		for (int i = 0; i < agents.size(); ++i)
		{
			std::unique_ptr<BaseAgent> &agent_ptr = agents[i];
			if (p == agent_ptr->agent_pid)
			{
				agent_ptr->is_alive = false;
			}
		}
	}
}

OpenraspAgentManager::OpenraspAgentManager(ShmManager *mm)
	: BaseManager(mm),
	  agent_ctrl_block(nullptr)
{
}

bool OpenraspAgentManager::startup()
{
	init_process_pid = getpid();
	if (check_sapi_need_alloc_shm())
	{
		if (!create_share_memory())
		{
			return false;
		}
		agent_ctrl_block->set_master_pid(init_process_pid);
		process_agent_startup();
		initialized = true;
	}
	return true;
}

bool OpenraspAgentManager::shutdown()
{
	if (initialized)
	{
		if (strcmp(sapi_module.name, "fpm-fcgi") == 0)
		{
			pid_t fpm_master_pid = search_fpm_master_pid();
			if (fpm_master_pid)
			{
				agent_ctrl_block->set_master_pid(fpm_master_pid);
			}
		}
		if (agent_ctrl_block->get_master_pid() && getpid() != agent_ctrl_block->get_master_pid())
		{
			return true;
		}
		process_agent_shutdown();
		destroy_share_memory();
		initialized = false;
	}
	return true;
}

bool OpenraspAgentManager::verify_ini_correct()
{
	TSRMLS_FETCH();
	if (openrasp_ini.remote_management_enable && check_sapi_need_alloc_shm())
	{
		if (nullptr == openrasp_ini.backend_url)
		{
			openrasp_error(E_WARNING, CONFIG_ERROR, _("openrasp.backend_url is required when remote management is enabled."));
			return false;
		}
		if (nullptr == openrasp_ini.app_id)
		{
			openrasp_error(E_WARNING, CONFIG_ERROR, _("openrasp.app_id is required when remote management is enabled."));
			return false;
		}
		else
		{
			if (!regex_match(openrasp_ini.app_id, "^[0-9a-fA-F]{40}$"))
			{
				openrasp_error(E_WARNING, CONFIG_ERROR, _("openrasp.app_id must have 40 characters"));
				return false;
			}
		}
	}
	return true;
}

bool OpenraspAgentManager::create_share_memory()
{
	char *shm_block = shm_manager->create(SHMEM_SEC_CTRL_BLOCK, sizeof(OpenraspCtrlBlock));
	if (shm_block && (agent_ctrl_block = reinterpret_cast<OpenraspCtrlBlock *>(shm_block)))
	{
		return true;
	}
	return false;
}

bool OpenraspAgentManager::destroy_share_memory()
{
	agent_ctrl_block = nullptr;
	this->shm_manager->destroy(SHMEM_SEC_CTRL_BLOCK);
	return true;
}

bool OpenraspAgentManager::process_agent_startup()
{
	if (openrasp_ini.plugin_update_enable)
	{
		agents.push_back(std::move((std::unique_ptr<BaseAgent>)new HeartBeatAgent()));
	}
	agents.push_back(std::move((std::unique_ptr<BaseAgent>)new LogAgent()));
	pid_t pid = fork();
	if (pid < 0)
	{
		return false;
	}
	else if (pid == 0)
	{
		setsid();
		supervisor_run();
	}
	else
	{
		agent_ctrl_block->set_supervisor_id(pid);
	}
	return true;
}

void OpenraspAgentManager::process_agent_shutdown()
{
	agents.clear();
	pid_t log_agent_id = agent_ctrl_block->get_log_agent_id();
	if (log_agent_id > 0)
	{
		kill(log_agent_id, SIGKILL);
	}
	if (openrasp_ini.plugin_update_enable)
	{
		pid_t plugin_agent_id = agent_ctrl_block->get_plugin_agent_id();
		if (plugin_agent_id > 0)
		{
			kill(plugin_agent_id, SIGKILL);
		}
	}
	pid_t supervisor_id = agent_ctrl_block->get_supervisor_id();
	if (supervisor_id > 0)
	{
		kill(supervisor_id, SIGKILL);
	}
}

void OpenraspAgentManager::supervisor_run()
{
	AGENT_SET_PROC_NAME("rasp-supervisor");
	struct sigaction sa_usr = {0};
	sa_usr.sa_flags = 0;
	sa_usr.sa_handler = supervisor_sigchld_handler;
	sigaction(SIGCHLD, &sa_usr, NULL);

	super_install_signal_handler();
	TSRMLS_FETCH();
	while (true)
	{
		for (int i = 0; i < task_interval; ++i)
		{
			if (i % task_interval == 0 && !has_registered)
			{
				has_registered = agent_remote_register();
			}
			if (i % 10 == 0 && has_registered)
			{
				check_work_processes_survival();
			}
			sleep(1);
			struct stat sb;
			if (VCWD_STAT(("/proc/" + std::to_string(agent_ctrl_block->get_master_pid())).c_str(), &sb) == -1 &&
				errno == ENOENT)
			{
				process_agent_shutdown();
			}
		}
	}
}

void OpenraspAgentManager::check_work_processes_survival()
{
	for (int i = 0; i < agents.size(); ++i)
	{
		std::unique_ptr<BaseAgent> &agent_ptr = agents[i];
		if (!agent_ptr->is_alive)
		{
			pid_t pid = fork();
			if (pid == 0)
			{
				agent_ptr->run();
			}
			else if (pid > 0)
			{
				agent_ptr->is_alive = true;
				agent_ptr->agent_pid = pid;
				agent_ptr->write_pid_to_shm(pid);
			}
		}
	}
}

bool OpenraspAgentManager::calculate_rasp_id()
{
	std::vector<std::string> hw_addrs;
	fetch_hw_addrs(hw_addrs);
	if (hw_addrs.empty())
	{
		return false;
	}
	std::string buf;
	for (auto hw_addr : hw_addrs)
	{
		buf += hw_addr;
	}
	buf += std::string(openrasp_ini.root_dir);
	this->rasp_id = md5sum(static_cast<const void *>(buf.c_str()), buf.length());
	return true;
}

std::string OpenraspAgentManager::get_rasp_id()
{
	return this->rasp_id;
}

char *OpenraspAgentManager::get_local_ip()
{
	return local_ip;
}

bool OpenraspAgentManager::agent_remote_register()
{
	TSRMLS_FETCH();
	if (!fetch_source_in_ip_packets(local_ip, sizeof(local_ip), openrasp_ini.backend_url))
	{
		sprintf(local_ip, "");
	}
	if (!calculate_rasp_id())
	{
		return false;
	}
	CURL *curl = curl_easy_init();
	if (!curl)
	{
		return false;
	}
	ResponseInfo res_info;
	std::string url_string = std::string(openrasp_ini.backend_url) + "/v1/agent/rasp";
	zval *body = nullptr;
	MAKE_STD_ZVAL(body);
	array_init(body);
	add_assoc_string(body, "id", (char *)rasp_id.c_str(), 1);
	char host_name[255] = {0};
	if (gethostname(host_name, sizeof(host_name) - 1))
	{
		sprintf(host_name, "UNKNOWN_HOST");
	}
	add_assoc_string(body, "host_name", host_name, 1);
	add_assoc_string(body, "language", "PHP", 1);
	add_assoc_string(body, "language_version", OPENRASP_PHP_VERSION, 1);
	add_assoc_string(body, "server_type", sapi_module.name, 1);
	add_assoc_string(body, "server_version", OPENRASP_PHP_VERSION, 1);
	add_assoc_string(body, "rasp_home", openrasp_ini.root_dir, 1);
	add_assoc_string(body, "local_ip", local_ip, 1);
	add_assoc_string(body, "version", PHP_OPENRASP_VERSION, 1);
	smart_str buf_json = {0};
	php_json_encode(&buf_json, body, 0 TSRMLS_CC);
	if (buf_json.a > buf_json.len)
	{
		buf_json.c[buf_json.len] = '\0';
		buf_json.len++;
	}
	perform_curl(curl, url_string, buf_json.c, res_info);
	smart_str_free(&buf_json);
	zval_ptr_dtor(&body);
	if (CURLE_OK != res_info.res)
	{
		openrasp_error(E_WARNING, AGENT_ERROR, _("Agent register error, CURL error code: %d."), res_info.res);
		return false;
	}
	zval *return_value = nullptr;
	MAKE_STD_ZVAL(return_value);
	php_json_decode(return_value, (char *)res_info.response_string.c_str(), res_info.response_string.size(), 1, 512 TSRMLS_CC);
	if (Z_TYPE_P(return_value) != IS_ARRAY)
	{
		zval_ptr_dtor(&return_value);
		return false;
	}
	if (res_info.response_code >= 200 && res_info.response_code < 300)
	{
		long status;
		bool has_status = fetch_outmost_long_from_ht(Z_ARRVAL_P(return_value), "status", &status);
		char *description = fetch_outmost_string_from_ht(Z_ARRVAL_P(return_value), "description");
		if (has_status && description)
		{
			if (0 == status)
			{
				zval_ptr_dtor(&return_value);
				return true;
			}
			else
			{
				openrasp_error(E_WARNING, AGENT_ERROR, _("Agent register error, status: %ld, description : %s."),
							   status, description);
			}
		}
	}
	else
	{
		openrasp_error(E_WARNING, AGENT_ERROR, _("Agent register error, response code: %ld."), res_info.response_code);
	}
	zval_ptr_dtor(&return_value);
	return false;
}

pid_t OpenraspAgentManager::search_fpm_master_pid()
{
	TSRMLS_FETCH();
	std::vector<std::string> processes;
	openrasp_scandir("/proc", processes,
					 [](const char *filename) {
						 TSRMLS_FETCH();
						 struct stat sb;
						 if (VCWD_STAT(("/proc/" + std::string(filename)).c_str(), &sb) == 0 && (sb.st_mode & S_IFDIR) != 0)
						 {
							 return true;
						 }
						 return false;
					 });
	for (std::string pid : processes)
	{
		std::string stat_file_path = "/proc/" + pid + "/stat";
		if (VCWD_ACCESS(stat_file_path.c_str(), F_OK) == 0)
		{
			std::ifstream ifs_stat(stat_file_path);
			std::string stat_line;
			std::getline(ifs_stat, stat_line);
			if (stat_line.empty())
			{
				continue;
			}
			std::stringstream stat_items(stat_line);
			std::string item;
			for (int i = 0; i < 4; ++i)
			{
				std::getline(stat_items, item, ' ');
			}
			int ppid = std::atoi(item.c_str());
			if (ppid == init_process_pid)
			{
				std::string cmdline_file_path = "/proc/" + pid + "/cmdline";
				std::ifstream ifs_cmdline(cmdline_file_path);
				std::string cmdline;
				std::getline(ifs_cmdline, cmdline);
				if (cmdline.find("php-fpm: master process") == 0)
				{
					return std::atoi(pid.c_str());
				}
			}
		}
	}
	return 0;
}

} // namespace openrasp
