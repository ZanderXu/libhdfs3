/********************************************************************
 * Copyright (c) 2013 - 2014, Pivotal Inc.
 * All rights reserved.
 *
 * Author: Zhanwei Wang
 ********************************************************************/
/********************************************************************
 * 2014 -
 * open source under Apache License Version 2.0
 ********************************************************************/
/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Exception.h"
#include "ExceptionInternal.h"
#include "Logger.h"
#include "NamenodeImpl.h"
#include "NamenodeProxy.h"
#include "StringUtil.h"

#include <string>
#include <random>

namespace Hdfs {
namespace Internal {

NamenodeProxy::NamenodeProxy(const std::vector<NamenodeInfo> & namenodeInfos, const std::string & tokenService,
                             const SessionConfig & c, const RpcAuth & a) :
    clusterid(tokenService), currentNamenode(0) {
    if (namenodeInfos.size() == 1) {
        enableNamenodeHA = false;
        maxNamenodeHARetry = 0;
    } else {
        enableNamenodeHA = true;
        maxNamenodeHARetry = c.getRpcMaxHaRetry();
    }

    for (size_t i = 0; i < namenodeInfos.size(); ++i) {
        std::vector<std::string> nninfo = StringSplit(namenodeInfos[i].getRpcAddr(), ":");

        if (nninfo.size() != 2) {
            THROW(InvalidParameter, "Cannot create namenode proxy, %s does not contain host or port",
                  namenodeInfos[i].getRpcAddr().c_str());
        }

        namenodes.push_back(
            shared_ptr<Namenode>(
                new NamenodeImpl(nninfo[0].c_str(), nninfo[1].c_str(), clusterid, c, a)));
    }

    // shuffler
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::shuffle(namenodes.begin(), namenodes.end(), std::default_random_engine(seed));
}

NamenodeProxy::~NamenodeProxy() {
}

shared_ptr<Namenode> NamenodeProxy::getActiveNamenode(uint32_t & oldValue) {
    lock_guard<mutex> lock(mut);

    if (namenodes.empty()) {
        THROW(HdfsFileSystemClosed, "NamenodeProxy is closed.");
    }

    oldValue = currentNamenode;
    return namenodes[currentNamenode % namenodes.size()];
}

void NamenodeProxy::failoverToNextNamenode(uint32_t oldValue) {
    lock_guard<mutex> lock(mut);

    if (oldValue != currentNamenode) {
        //already failover in another thread.
        return;
    }

    ++currentNamenode;
    currentNamenode = currentNamenode % namenodes.size();
}

static void HandleHdfsFailoverException(const HdfsFailoverException & e) {
    try {
        Hdfs::rethrow_if_nested(e);
    } catch (...) {
        NESTED_THROW(Hdfs::HdfsRpcException, "%s", e.what());
    }

    //should not reach here
    abort();
}

#define NAMENODE_HA_RETRY_BEGIN() \
    do { \
        int __count = 0; \
        do { \
            uint32_t __oldValue = 0; \
            shared_ptr<Namenode> namenode =  getActiveNamenode(__oldValue); \
            try { \
                (void)0

#define NAMENODE_HA_RETRY_END() \
    break; \
    } catch (const NameNodeStandbyException & e) { \
        if (!enableNamenodeHA || __count++ > maxNamenodeHARetry) { \
            LOG(LOG_ERROR, "NamenodeProxy: Cannot failover to another NameNode, retry count is %d.", __count); \
            throw; \
        } \
    } catch (const HdfsFailoverException & e) { \
        if (!enableNamenodeHA || __count++ > maxNamenodeHARetry) { \
            LOG(LOG_ERROR, "NamenodeProxy: Cannot failover to another NameNode, retry count is %d", __count);  \
            HandleHdfsFailoverException(e); \
        } \
    } \
    failoverToNextNamenode(__oldValue); \
    LOG(WARNING, "NamenodeProxy: Failover to another Namenode, retry count is %d.", __count); \
    } while (true); \
    } while (0)

void NamenodeProxy::getBlockLocations(const std::string & src, int64_t offset,
                                      int64_t length, LocatedBlocks & lbs) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->getBlockLocations(src, offset, length, lbs);
    NAMENODE_HA_RETRY_END();
}

FileStatus NamenodeProxy::create(const std::string & src, const Permission & masked,
                                 const std::string & clientName, int flag, bool createParent,
                                 short replication, int64_t blockSize) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->create(src, masked, clientName, flag, createParent, replication, blockSize);
    NAMENODE_HA_RETRY_END();
}

std::pair<shared_ptr<LocatedBlock>, shared_ptr<FileStatus> >
NamenodeProxy::append(const std::string& src, const std::string& clientName, const uint32_t& flag) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->append(src, clientName, flag);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return std::pair<shared_ptr<LocatedBlock>, shared_ptr<FileStatus> >();
}

bool NamenodeProxy::setReplication(const std::string & src, short replication) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->setReplication(src, replication);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return false;
}

void NamenodeProxy::setPermission(const std::string & src,
                                  const Permission & permission) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->setPermission(src, permission);
    NAMENODE_HA_RETRY_END();
}

void NamenodeProxy::setOwner(const std::string & src,
                             const std::string & username, const std::string & groupname) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->setOwner(src, username, groupname);
    NAMENODE_HA_RETRY_END();
}

void NamenodeProxy::abandonBlock(const ExtendedBlock & b, const std::string & src,
                                 const std::string & holder, int64_t fileId) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->abandonBlock(b, src, holder, fileId);
    NAMENODE_HA_RETRY_END();
}

shared_ptr<LocatedBlock> NamenodeProxy::addBlock(const std::string & src,
        const std::string & clientName, const ExtendedBlock * previous,
        const std::vector<DatanodeInfo> & excludeNodes, int64_t fileId) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->addBlock(src, clientName, previous, excludeNodes, fileId);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return shared_ptr<LocatedBlock>();
}

shared_ptr<LocatedBlock> NamenodeProxy::getAdditionalDatanode(
    const std::string & src, const ExtendedBlock & blk,
    const std::vector<DatanodeInfo> & existings,
    const std::vector<std::string> & storageIDs,
    const std::vector<DatanodeInfo> & excludes, int numAdditionalNodes,
    const std::string & clientName) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getAdditionalDatanode(src, blk, existings,
                                           storageIDs, excludes, numAdditionalNodes, clientName);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return shared_ptr<LocatedBlock>();
}

bool NamenodeProxy::complete(const std::string & src,
                             const std::string & clientName, const ExtendedBlock * last, int64_t fileId) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->complete(src, clientName, last, fileId);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return false;
}

/*void NamenodeProxy::reportBadBlocks(const std::vector<LocatedBlock> & blocks) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->reportBadBlocks(blocks);
    NAMENODE_HA_RETRY_END();
}*/

bool NamenodeProxy::rename(const std::string & src, const std::string & dst) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->rename(src, dst);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return false;
}

/*
void NamenodeProxy::concat(const std::string & trg,
                           const std::vector<std::string> & srcs) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->concat(trg, srcs);
    NAMENODE_HA_RETRY_END();
}
*/

bool NamenodeProxy::truncate(const std::string & src, int64_t size,
                             const std::string & clientName) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->truncate(src, size, clientName);
    NAMENODE_HA_RETRY_END();
}

void NamenodeProxy::getLease(const std::string & src,
                             const std::string & clientName) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->getLease(src, clientName);
    NAMENODE_HA_RETRY_END();
}

void NamenodeProxy::releaseLease(const std::string & src,
                                 const std::string & clientName) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->releaseLease(src, clientName);
    NAMENODE_HA_RETRY_END();
}

bool NamenodeProxy::deleteFile(const std::string & src, bool recursive) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->deleteFile(src, recursive);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return false;
}

bool NamenodeProxy::mkdirs(const std::string & src, const Permission & masked,
                           bool createParent) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->mkdirs(src, masked, createParent);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return false;
}

bool NamenodeProxy::getListing(const std::string & src,
                               const std::string & startAfter, bool needLocation,
                               std::vector<FileStatus> & dl) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getListing(src, startAfter, needLocation, dl);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return false;
}

void NamenodeProxy::renewLease(const std::string & clientName) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->renewLease(clientName);
    NAMENODE_HA_RETRY_END();
}

/*bool NamenodeProxy::recoverLease(const std::string & src,
                                 const std::string & clientName) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->recoverLease(src, clientName);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return false;
}*/

std::vector<int64_t> NamenodeProxy::getFsStats() {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getFsStats();
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return std::vector<int64_t>();
}

/*void NamenodeProxy::metaSave(const std::string & filename) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->metaSave(filename);
    NAMENODE_HA_RETRY_END();
}*/

FileStatus NamenodeProxy::getFileInfo(const std::string & src) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getFileInfo(src);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return FileStatus();
}

/*FileStatus NamenodeProxy::getFileLinkInfo(const std::string & src) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getFileLinkInfo(src);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return FileStatus();
}*/

/*ContentSummary NamenodeProxy::getContentSummary(const std::string & path) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getContentSummary(path);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return ContentSummary();
}*/

/*void NamenodeProxy::setQuota(const std::string & path, int64_t namespaceQuota,
                             int64_t diskspaceQuota) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->setQuota(path, namespaceQuota, diskspaceQuota);
    NAMENODE_HA_RETRY_END();
}*/

void NamenodeProxy::fsync(const std::string & src, const std::string & client) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->fsync(src, client);
    NAMENODE_HA_RETRY_END();
}

void NamenodeProxy::setTimes(const std::string & src, int64_t mtime,
                             int64_t atime) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->setTimes(src, mtime, atime);
    NAMENODE_HA_RETRY_END();
}

/*void NamenodeProxy::createSymlink(const std::string & target,
                                  const std::string & link, const Permission & dirPerm,
                                  bool createParent) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->createSymlink(target, link, dirPerm, createParent);
    NAMENODE_HA_RETRY_END();
}*/

/*std::string NamenodeProxy::getLinkTarget(const std::string & path) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getLinkTarget(path);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return "";
}*/

shared_ptr<LocatedBlock> NamenodeProxy::updateBlockForPipeline(
    const ExtendedBlock & block, const std::string & clientName) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->updateBlockForPipeline(block, clientName);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return shared_ptr<LocatedBlock>();
}

void NamenodeProxy::updatePipeline(const std::string & clientName,
                                   const ExtendedBlock & oldBlock, const ExtendedBlock & newBlock,
                                   const std::vector<DatanodeInfo> & newNodes,
                                   const std::vector<std::string> & storageIDs) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->updatePipeline(clientName, oldBlock, newBlock, newNodes, storageIDs);
    NAMENODE_HA_RETRY_END();
}

Token NamenodeProxy::getDelegationToken(const std::string & renewer) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->getDelegationToken(renewer);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return Token();
}

int64_t NamenodeProxy::renewDelegationToken(const Token & token) {
    NAMENODE_HA_RETRY_BEGIN();
    return namenode->renewDelegationToken(token);
    NAMENODE_HA_RETRY_END();
    assert(!"should not reach here");
    return 0;
}

void NamenodeProxy::cancelDelegationToken(const Token & token) {
    NAMENODE_HA_RETRY_BEGIN();
    namenode->cancelDelegationToken(token);
    NAMENODE_HA_RETRY_END();
}

void NamenodeProxy::close() {
    lock_guard<mutex> lock(mut);
    namenodes.clear();
}

}
}
