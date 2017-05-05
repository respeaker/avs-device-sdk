/*
* DirectiveSequencer.cpp
*
* Copyright 2017 Amazon.com, Inc. or its affiliates.
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

#include <algorithm>
#include <iostream>
#include <sstream>

#include <AVSCommon/ExceptionEncountered.h>
#include <AVSUtils/Logger/LogEntry.h>
#include <AVSUtils/Logging/Logger.h>

#include "ADSL/DirectiveSequencer.h"

/// String to identify log entries originating from this file.
static const std::string TAG("DirectiveSequencer");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsUtils::logger::LogEntry(TAG, event)

namespace alexaClientSDK {
namespace adsl {

using namespace avsCommon;

std::unique_ptr<DirectiveSequencerInterface> DirectiveSequencer::create(
        std::shared_ptr<avsCommon::ExceptionEncounteredSenderInterface> exceptionSender) {
    if (!exceptionSender) {
        ACSDK_INFO(LX("createFailed").d("reason", "nullptrExceptionSender"));
        return nullptr;
    }
    return std::unique_ptr<DirectiveSequencerInterface>(new DirectiveSequencer(exceptionSender));
}

DirectiveSequencer::~DirectiveSequencer() {
    shutdown();
}

void DirectiveSequencer::shutdown() {
    ACSDK_INFO(LX("shutdown"));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isShuttingDown = true;
        m_wakeReceivingLoop.notify_one();
    }
    if (m_receivingThread.joinable()) {
        m_receivingThread.join();
    }
    m_directiveProcessor->shutdown();
}

bool DirectiveSequencer::addDirectiveHandlers(const DirectiveHandlerConfiguration& configuration) {
    return m_directiveRouter.addDirectiveHandlers(configuration);
}

bool DirectiveSequencer::removeDirectiveHandlers(const DirectiveHandlerConfiguration& configuration) {
    return m_directiveRouter.removeDirectiveHandlers(configuration);
}

void DirectiveSequencer::setDialogRequestId(const std::string& dialogRequestId) {
    m_directiveProcessor->setDialogRequestId(dialogRequestId);
}

bool DirectiveSequencer::onDirective(std::shared_ptr<AVSDirective> directive) {
    if (!directive) {
        ACSDK_ERROR(LX("onDirectiveFailed").d("action", "ignored").d("reason", "nullptrDirective"));
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isShuttingDown) {
        ACSDK_WARN(LX("onDirectiveFailed")
                .d("directive", directive->getHeaderAsString())
                .d("action", "ignored")
                .d("reason", "isShuttingDown"));
        return false;
    }
    ACSDK_INFO(LX("onDirective").d("directive", directive->getHeaderAsString()));
    m_receivingQueue.push_back(directive);
    m_wakeReceivingLoop.notify_one();
    return true;
}

DirectiveSequencer::DirectiveSequencer(
        std::shared_ptr<avsCommon::ExceptionEncounteredSenderInterface> exceptionSender) :
        m_mutex{},
        m_exceptionSender{exceptionSender},
        m_isShuttingDown{false} {
    m_directiveProcessor = std::make_shared<DirectiveProcessor>(&m_directiveRouter);
    m_receivingThread = std::thread(&DirectiveSequencer::receivingLoop, this);
}

void DirectiveSequencer::receivingLoop() {
    auto wake = [this]() {
        return !m_receivingQueue.empty() || m_isShuttingDown;
    };

    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_wakeReceivingLoop.wait(lock, wake);
        if (m_isShuttingDown) {
            break;
        }
        receiveDirectiveLocked(lock);
    }
}

void DirectiveSequencer::receiveDirectiveLocked(std::unique_lock<std::mutex> &lock) {
    if (m_receivingQueue.empty()) {
        return;
    }
    auto directive = m_receivingQueue.front();
    m_receivingQueue.pop_front();
    lock.unlock();
    bool handled = false;
    if (directive->getDialogRequestId().empty()) {
        handled = m_directiveRouter.handleDirectiveImmediately(directive);
    } else  {
        handled = m_directiveProcessor->onDirective(directive);
    }
    if (!handled) {
        ACSDK_INFO(LX("sendingExceptionEncountered").d("messageId", directive->getMessageId()));
        m_exceptionSender->sendExceptionEncountered(
                directive->getUnparsedDirective(),
                avsCommon::ExceptionErrorType::UNSUPPORTED_OPERATION,
                "Unsupported operation");
    }
    lock.lock();
}

} // namespace directiveSequencer
} // namespace alexaClientSDK