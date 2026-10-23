// SPDX-License-Identifier: GPL-2.0-only
#include <array>
#include <cstdio>
#include <stdexcept>

#include "core/libraries/system/msgdialog.h"
#include "core/libraries/system/msgdialog_ui.h"
#include "../../shared/dialog/NetworkNoticePolicy.h"

using namespace Libraries::CommonDialog;
using namespace Libraries::MsgDialog;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        // Connectivity wording and feature list from the observed guest notice;
        // no title identity or feature names participate in the policy.
        constexpr char offline_notice[] =
            "You are not currently connected to Internet.\n"
            "The following features won’t be available till you reconnect:\n"
            "- Leaderboards\n- CrossSave";
        Check(Lumen::Dialog::IsOfflineConnectivityNotice(offline_notice), "observed notice matched");
        Check(Lumen::Dialog::IsOfflineConnectivityNotice("You ARE NOT\nCONNECTED\tTO THE INTERNET."),
              "case and whitespace normalized");
        Check(Lumen::Dialog::IsOfflineConnectivityNotice("You aren't connected to Internet."),
              "contracted offline notice");
        Check(Lumen::Dialog::IsOfflineConnectivityNotice("The console isn’t connected to the Internet."),
              "typographic apostrophe normalized");
        Check(Lumen::Dialog::IsOfflineConnectivityNotice("NON SEI CONNESSO A INTERNET."),
              "Italian offline notice");
        Check(!Lumen::Dialog::IsOfflineConnectivityNotice("You are connected to the Internet."),
              "online notification retained");
        Check(!Lumen::Dialog::IsOfflineConnectivityNotice("Unable to load saved data."),
              "unrelated error retained");
        Check(!Lumen::Dialog::IsOfflineConnectivityNotice("not connected to internet-enabled storage"),
              "unrelated resource names retained");

        Check(sceMsgDialogInitialize() == Error::NOT_SYSTEM_INITIALIZED, "system init required");
        Check(sceCommonDialogInitialize() == Error::OK, "system initialized");
        Check(sceMsgDialogInitialize() == Error::OK, "message initialized");
        Check(sceCommonDialogIsUsed(), "common dialog ownership acquired");
        DialogResult result{};
        Check(sceMsgDialogGetResult(&result) == Error::NOT_FINISHED, "initial result unavailable");

        UserMessageParam user{};
        user.buttonType = ButtonType::OK;
        user.msg = offline_notice;
        OrbisParam param{};
        param.baseParam.size = sizeof(BaseParam);
        param.size = sizeof(OrbisParam);
        param.mode = MsgDialogMode::USER_MSG;
        param.userMsgParam = &user;
        Check(sceMsgDialogOpen(&param) == Error::OK, "offline notice opened");
        Check(sceMsgDialogGetStatus() == Status::FINISHED &&
                  sceMsgDialogUpdateStatus() == Status::FINISHED, "offline notice completed without UI");
        Check(sceMsgDialogGetResult(nullptr) == Error::ARG_NULL, "null result checked");
        Check(sceMsgDialogGetResult(&result) == Error::OK && result.result == Result::OK &&
                  result.buttonId == ButtonId::OK, "same result as OK acknowledgement");
        result.buttonId = ButtonId::INVALID;
        Check(sceMsgDialogGetResult(&result) == Error::OK && result.buttonId == ButtonId::OK,
              "result retained across reads");
        Check(sceMsgDialogClose() == Error::NOT_RUNNING, "completed close reports existing status");
        Check(sceCommonDialogIsUsed(), "completed result retains ownership until terminate");

        user.msg = "Unable to load saved data.";
        Check(sceMsgDialogOpen(&param) == Error::OK, "unrelated OK message reopened");
        Check(sceMsgDialogGetStatus() == Status::RUNNING, "unrelated OK message stays visible");
        Check(sceMsgDialogGetResult(&result) == Error::NOT_FINISHED, "new dialog clears old result");
        Check(sceMsgDialogClose() == Error::OK, "ordinary message closes");
        Check(sceMsgDialogGetResult(&result) == Error::OK && result.buttonId == ButtonId::INVALID,
              "ordinary close semantics preserved");

        user.msg = offline_notice;
        for (const auto type : std::array{ButtonType::YESNO, ButtonType::NONE, ButtonType::OK_CANCEL,
                                        ButtonType::WAIT, ButtonType::WAIT_CANCEL,
                                        ButtonType::YESNO_FOCUS_NO, ButtonType::OK_CANCEL_FOCUS_CANCEL}) {
            user.buttonType = type;
            Check(sceMsgDialogOpen(&param) == Error::OK, "non-acknowledgement message opened");
            Check(sceMsgDialogGetStatus() == Status::RUNNING, "question/cancel/wait message stays visible");
            Check(sceMsgDialogClose() == Error::OK, "question/cancel/wait lifecycle preserved");
        }
        ButtonsParam buttons{};
        buttons.msg1 = "Retry";
        buttons.msg2 = "Continue offline";
        user.buttonType = ButtonType::TWO_BUTTONS;
        user.buttonsParam = &buttons;
        Check(sceMsgDialogOpen(&param) == Error::OK && sceMsgDialogGetStatus() == Status::RUNNING,
              "custom choices stay visible");
        Check(sceMsgDialogClose() == Error::OK, "custom choices close normally");

        ProgressBarParam progress{};
        progress.msg = offline_notice;
        param.mode = MsgDialogMode::PROGRESS_BAR;
        param.progBarParam = &progress;
        Check(sceMsgDialogOpen(&param) == Error::OK && sceMsgDialogGetStatus() == Status::RUNNING,
              "same-text progress stays running");
        Check(sceMsgDialogProgressBarSetValue(OrbisMsgDialogProgressBarTarget::DEFAULT, 50) == Error::OK,
              "progress API remains operational");
        Check(sceMsgDialogClose() == Error::OK, "progress closes normally");

        SystemMessageParam system{};
        system.sysMsgType = SystemMessageType::CAMERA_NOT_CONNECTED;
        param.mode = MsgDialogMode::SYSTEM_MSG;
        param.sysMsgParam = &system;
        Check(sceMsgDialogOpen(&param) == Error::OK && sceMsgDialogGetStatus() == Status::RUNNING,
              "typed system notice retained");
        Check(sceMsgDialogClose() == Error::OK, "system notice closes normally");

        param.mode = MsgDialogMode::USER_MSG;
        user.buttonType = ButtonType::OK;
        Check(sceMsgDialogOpen(&param) == Error::OK && sceMsgDialogGetStatus() == Status::FINISHED,
              "offline notice can reopen after unrelated dialogs");
        Check(sceMsgDialogTerminate() == Error::OK && !sceCommonDialogIsUsed(), "terminate releases ownership");
        Check(sceMsgDialogGetStatus() == Status::NONE && sceMsgDialogGetResult(&result) == Error::NOT_FINISHED,
              "terminated result unavailable");
        Check(sceMsgDialogInitialize() == Error::OK, "new session initializes");
        Check(sceMsgDialogOpen(&param) == Error::OK && sceMsgDialogGetStatus() == Status::FINISHED,
              "new session auto-acknowledges");
        Check(sceMsgDialogTerminate() == Error::OK, "new session terminates");

        const auto host_result = ShowMsgDialog(MsgDialogState{MsgDialogState::UserState{
            .type = ButtonType::OK, .msg = offline_notice}}, true);
        Check(host_result.result == Result::OK && host_result.buttonId == ButtonId::OK,
              "shared blocking host helper acknowledges without waiting");
        std::puts("Gen4 network notice classification and dialog lifecycle contract PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
