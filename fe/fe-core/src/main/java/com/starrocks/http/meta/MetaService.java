// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/http/meta/MetaService.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.http.meta;

import com.google.common.base.Strings;
import com.google.gson.Gson;
import com.starrocks.catalog.Catalog;
import com.starrocks.common.Config;
import com.starrocks.ha.FrontendNodeType;
import com.starrocks.http.ActionController;
import com.starrocks.http.BaseRequest;
import com.starrocks.http.BaseResponse;
import com.starrocks.http.IllegalArgException;
import com.starrocks.master.MetaHelper;
import com.starrocks.persist.MetaCleaner;
import com.starrocks.persist.Storage;
import com.starrocks.persist.StorageInfo;
import com.starrocks.system.Frontend;
import io.netty.handler.codec.http.HttpMethod;
import io.netty.handler.codec.http.HttpResponseStatus;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.OutputStream;

public class MetaService {
    private static final int TIMEOUT_SECOND = 10;

    public static class ImageAction extends MetaBaseAction {
        private static final String VERSION = "version";

        public ImageAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/image", new ImageAction(controller, imageDir));
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            String versionStr = request.getSingleParameter(VERSION);
            if (Strings.isNullOrEmpty(versionStr)) {
                response.appendContent("Miss version parameter");
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                return;
            }

            long version = checkLongParam(versionStr);
            if (version < 0) {
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                return;
            }

            File imageFile = Storage.getImageFile(imageDir, version);
            if (!imageFile.exists()) {
                writeResponse(request, response, HttpResponseStatus.NOT_FOUND);
                return;
            }

            writeFileResponse(request, response, imageFile);
        }
    }

    public static class InfoAction extends MetaBaseAction {
        private static final Logger LOG = LogManager.getLogger(InfoAction.class);

        public InfoAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/info", new InfoAction(controller, imageDir));
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            try {
                Storage currentStorageInfo = new Storage(imageDir.getAbsolutePath());
                StorageInfo storageInfo = new StorageInfo(currentStorageInfo.getClusterID(),
                        currentStorageInfo.getImageSeq(), currentStorageInfo.getEditsSeq());

                response.setContentType("application/json");
                Gson gson = new Gson();
                response.appendContent(gson.toJson(storageInfo));
                writeResponse(request, response);
                return;
            } catch (IOException e) {
                LOG.warn("IO error.", e);
                response.appendContent("failed to get master info.");
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                return;
            }
        }
    }

    public static class VersionAction extends MetaBaseAction {
        public VersionAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/version", new VersionAction(controller, imageDir));
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            File versionFile = new File(imageDir, Storage.VERSION_FILE);
            writeFileResponse(request, response, versionFile);
        }
    }

    public static class PutAction extends MetaBaseAction {
        private static final Logger LOG = LogManager.getLogger(PutAction.class);

        private static final String VERSION = "version";
        private static final String PORT = "port";

        public PutAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/put", new PutAction(controller, imageDir));
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            String machine = request.getHostString();
            String portStr = request.getSingleParameter(PORT);
            // check port to avoid SSRF(Server-Side Request Forgery)
            if (Strings.isNullOrEmpty(portStr)) {
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                return;
            }
            {
                int port = Integer.parseInt(portStr);
                if (port < 0 || port > 65535) {
                    LOG.warn("port is invalid. port={}", port);
                    writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                    return;
                }
            }

            String versionStr = request.getSingleParameter(VERSION);
            if (Strings.isNullOrEmpty(versionStr)) {
                response.appendContent("Miss version parameter");
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                return;
            }
            long version = checkLongParam(versionStr);

            // for master node, reject image put
            if (Catalog.getCurrentCatalog().isMaster()) {
                response.appendContent("this node is master, reject image put");
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                LOG.error("this node is master, but receive image put from host{}, reject it", machine);
                return;
            }

            // do not accept image whose version is bigger than max journalId
            // if accepted, newly added log will not be replayed when restart
            long maxJournalId = Catalog.getCurrentCatalog().getMaxJournalId();
            if (version > maxJournalId) {
                response.appendContent("image version is bigger than local max journal id, reject image put");
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                LOG.error("receive image whose version [{}] is bigger than local max journal id [{}], reject it",
                        version, maxJournalId);
                return;
            }

            String url = "http://" + machine + ":" + portStr
                    + "/image?version=" + versionStr;
            String filename = Storage.IMAGE + "." + versionStr;

            File dir = new File(Catalog.getCurrentCatalog().getImageDir());
            try {
                OutputStream out = MetaHelper.getOutputStream(filename, dir);
                MetaHelper.getRemoteFile(url, TIMEOUT_SECOND * 1000, out);
                MetaHelper.complete(filename, dir);
                writeResponse(request, response);
            } catch (FileNotFoundException e) {
                LOG.warn("file not found. file: {}", filename, e);
                writeResponse(request, response, HttpResponseStatus.NOT_FOUND);
                return;
            } catch (IOException e) {
                LOG.warn("failed to get remote file. url: {}", url, e);
                writeResponse(request, response, HttpResponseStatus.INTERNAL_SERVER_ERROR);
                return;
            }

            // Delete old image files
            MetaCleaner cleaner = new MetaCleaner(Config.meta_dir + "/image");
            try {
                cleaner.clean();
            } catch (IOException e) {
                LOG.error("Follower/Observer delete old image file fail.", e);
            }
        }
    }

    public static class JournalIdAction extends MetaBaseAction {
        public JournalIdAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/journal_id", new JournalIdAction(controller, imageDir));
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            long id = Catalog.getCurrentCatalog().getReplayedJournalId();
            response.updateHeader("id", Long.toString(id));
            writeResponse(request, response);
        }
    }

    public static class RoleAction extends MetaBaseAction {
        private static final String HOST = "host";
        private static final String PORT = "port";

        public RoleAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/role", new RoleAction(controller, imageDir));
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            String host = request.getSingleParameter(HOST);
            String portString = request.getSingleParameter(PORT);

            if (!Strings.isNullOrEmpty(host) && !Strings.isNullOrEmpty(portString)) {
                int port = Integer.parseInt(portString);
                Frontend fe = Catalog.getCurrentCatalog().checkFeExist(host, port);
                if (fe == null) {
                    response.updateHeader("role", FrontendNodeType.UNKNOWN.name());
                } else {
                    response.updateHeader("role", fe.getRole().name());
                    response.updateHeader("name", fe.getNodeName());
                }
                writeResponse(request, response);
            } else {
                response.appendContent("Miss parameter");
                writeResponse(request, response, HttpResponseStatus.BAD_REQUEST);
                return;
            }
        }
    }

    /*
     * This action is used to get the electable_nodes config and the cluster id of
     * the fe with the given ip and port. When one frontend start, it should check
     * the local electable_nodes config and local cluster id with other frontends.
     * If there is any difference, local fe will exit. This is designed to protect
     * the consistency of the cluster.
     */
    public static class CheckAction extends MetaBaseAction {
        private static final Logger LOG = LogManager.getLogger(CheckAction.class);

        public CheckAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/check",
                    new CheckAction(controller, imageDir));
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            try {
                Storage storage = new Storage(imageDir.getAbsolutePath());
                response.updateHeader(MetaBaseAction.CLUSTER_ID, Integer.toString(storage.getClusterID()));
                response.updateHeader(MetaBaseAction.TOKEN, storage.getToken());
            } catch (IOException e) {
                LOG.error(e);
            }
            writeResponse(request, response);
        }
    }

    public static class DumpAction extends MetaBaseAction {
        private static final Logger LOG = LogManager.getLogger(CheckAction.class);

        public DumpAction(ActionController controller, File imageDir) {
            super(controller, imageDir);
        }

        public static void registerAction(ActionController controller, File imageDir)
                throws IllegalArgException {
            controller.registerHandler(HttpMethod.GET, "/dump", new DumpAction(controller, imageDir));
        }

        @Override
        public boolean needAdmin() {
            return true;
        }

        @Override
        protected boolean needCheckClientIsFe() {
            return false;
        }

        @Override
        public void executeGet(BaseRequest request, BaseResponse response) {
            /*
             * Before dump, we acquired the catalog read lock and all databases' read lock and all
             * the jobs' read lock. This will guarantee the consistency of database and job queues.
             * But Backend may still inconsistent.
             */

            // TODO: Still need to lock ClusterInfoService to prevent add or drop Backends
            String dumpFilePath = Catalog.getCurrentCatalog().dumpImage();
            if (dumpFilePath == null) {
                response.appendContent("dump failed. " + dumpFilePath);
            }

            response.appendContent("dump finished. " + dumpFilePath);
            writeResponse(request, response);
            return;
        }
    }
}
