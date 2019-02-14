/*
 * Copyright 2017-2019 Baidu Inc.
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

package com.baidu.openrasp.detector;

import com.baidu.openrasp.HookHandler;
import com.baidu.openrasp.cloud.Register;
import com.baidu.openrasp.cloud.model.AppenderMappedLogger;
import com.baidu.openrasp.cloud.model.CloudCacheModel;
import com.baidu.openrasp.cloud.syslog.DynamicConfigAppender;
import com.baidu.openrasp.cloud.utils.CloudUtils;
import com.baidu.openrasp.config.Config;
import com.baidu.openrasp.plugin.checker.CheckParameter;
import com.baidu.openrasp.tool.OSUtil;
import com.baidu.openrasp.tool.model.ApplicationModel;
import org.apache.log4j.Logger;

import java.security.ProtectionDomain;

/**
 * Created by tyy on 19-2-12.
 *
 * 服务器信息探测
 */
public abstract class ServerDetector {

    public static final Logger LOGGER = Logger.getLogger(ServerDetector.class.getName());

    /**
     * 探测该类是否为服务器标志类
     *
     * @param className   类名
     * @param classLoader 类的加载器
     */
    public void handleServer(String className, ClassLoader classLoader, ProtectionDomain domain) {
        handleServerInfo(classLoader, domain);
        if (isClassMatched(className)) {
            sendRegister();
        }
    }

    public abstract boolean isClassMatched(String className);

    public abstract void handleServerInfo(ClassLoader classLoader, ProtectionDomain domain);

    protected void sendRegister() {
        if (CloudUtils.checkCloudControlEnter()) {
            String cloudAddress = Config.getConfig().getCloudAddress();
            try {
                CloudCacheModel.getInstance().setMasterIp(OSUtil.getMasterIp(cloudAddress));
            } catch (Exception e) {
                LOGGER.warn("get local ip failed: ", e);
            }
            //初始化创建http appender
            DynamicConfigAppender.createRootHttpAppender();
            DynamicConfigAppender.createHttpAppender(AppenderMappedLogger.HTTP_ALARM.getLogger(),
                    AppenderMappedLogger.HTTP_ALARM.getAppender());
            DynamicConfigAppender.createHttpAppender(AppenderMappedLogger.HTTP_POLICY_ALARM.getLogger(),
                    AppenderMappedLogger.HTTP_POLICY_ALARM.getAppender());
            new Register();
        } else {
            checkServerPolicy();
        }
    }

    /**
     * 服务器基线检测
     */
    public static void checkServerPolicy() {
        String serverName = ApplicationModel.getServerName();
        if ("tomcat".equals(serverName)) {
            HookHandler.doPolicyCheckWithoutRequest(CheckParameter.Type.POLICY_SERVER_TOMCAT, CheckParameter.EMPTY_MAP);
        } else if ("jboss".equals(serverName)) {
            HookHandler.doPolicyCheckWithoutRequest(CheckParameter.Type.POLICY_SERVER_JBOSS, CheckParameter.EMPTY_MAP);
        } else if ("jetty".equals(serverName)) {
            HookHandler.doPolicyCheckWithoutRequest(CheckParameter.Type.POLICY_SERVER_JETTY, CheckParameter.EMPTY_MAP);
        } else if ("resin".equals(serverName)) {
            HookHandler.doPolicyCheckWithoutRequest(CheckParameter.Type.POLICY_SERVER_RESIN, CheckParameter.EMPTY_MAP);
        } else if ("websphere".equals(serverName)) {
            HookHandler.doPolicyCheckWithoutRequest(CheckParameter.Type.POLICY_SERVER_WEBSPHERE, CheckParameter.EMPTY_MAP);
        } else if ("weblogic".equals(serverName)) {
            HookHandler.doPolicyCheckWithoutRequest(CheckParameter.Type.POLICY_SERVER_WEBLOGIC, CheckParameter.EMPTY_MAP);
        }
    }

}