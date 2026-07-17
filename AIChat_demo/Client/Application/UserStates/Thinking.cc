#include "Thinking.h"
#include "../../Utils/user_log.h"
#include "../../Events/AppEvents.h"

void ThinkingState::Enter(Application* app) {
    std::string json_message = R"({"type": "state", "state": "thinking"})";
    app->ws_client_.SendText(json_message);
    app->set_first_audio_msg_received(true);

    PendingVisionQuestion pending_question;
    if (app->ConsumePendingVisionQuestion(pending_question)) {
        USER_LOG_INFO("Thinking state is dispatching pending vision request. source=%s",
                      pending_question.source.c_str());
        if (!app->SubmitVisionQuestion(pending_question.prompt, pending_question.source)) {
            USER_LOG_WARN("Pending vision request failed to submit, returning to idle.");
            app->eventQueue_.Enqueue(static_cast<int>(AppEvent::dialogue_end));
        }
    }

    USER_LOG_INFO("Into thinking state.");
}

void ThinkingState::Exit(Application* app) {
    USER_LOG_INFO("thinking state exit.");
}
