/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WTFTestUtilities.h"

#include <wtf/Assertions.h>

namespace TestWebKitAPI {

#if OS(DARWIN)

constexpr int SIG_WK_WAITPID_ERROR = 101; // Signal for error when waiting for pid.

// Taken from dt_waitpid() in libdarwintest.
static bool wkWaitPID(pid_t pid, int* exitStatus, int* signalNumber)
{
    int status = 0;

    auto handleError = ^{
        LOG_ERROR("Return SIG_WK_WAITPID_ERROR for error during wkWaitPID()");
        if (exitStatus)
            *exitStatus = 0;
        if (signalNumber)
            *signalNumber = SIG_WK_WAITPID_ERROR;
    };

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        LOG_ERROR("waitpid(): errno=%d (%s)", errno, strerror(errno));
        handleError();
        return false;
    }

    if (WIFEXITED(status)) {
        if (exitStatus)
            *exitStatus = WEXITSTATUS(status);
        if (signalNumber)
            *signalNumber = 0;
        return !WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        if (exitStatus)
            *exitStatus = 0;
        if (signalNumber)
            *signalNumber = WTERMSIG(status);
        return false;
    }

    handleError();
    return false;
}

static void signalHandler(int signal)
{
    exit(signal);
}

static void installSignalHandlers()
{
    signal(SIGTRAP, signalHandler);
}

void expectReleaseAssert(NOESCAPE const WTF::Function<void()>& crashingFunction)
{
    pid_t pid = fork();

    EXPECT_NE(pid, -1);

    if (!pid) {
        // Child process.
        installSignalHandlers();
        crashingFunction();
        exit(0); // Failure since we expect a crash.
    }

    int status = 0, signal = 0;
    bool isSuccessfulExit = wkWaitPID(pid, &status, &signal);
    EXPECT_TRUE(!isSuccessfulExit); // Expect to exit with non-zero status.
    EXPECT_FALSE(WIFSIGNALED(signal)); // Child should not crash because signal was caught.
    EXPECT_EQ(status, SIGTRAP); // Check for CRASH() from <wtf/Assertions.h>.
}

#endif // OS(DARWIN)

} // namespace TestWebKitAPI
