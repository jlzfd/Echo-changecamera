#include "WS_Handler.h"
#include "../Utils/user_log.h"
#include "Application.h"

namespace {

bool HasRequestId(const Json::Value& root) {
    return root.isMember("request_id") && root["request_id"].isString();
}

bool IsStaleRequest(const Json::Value& root, Application* app) {
    return HasRequestId(root) && !app->IsActiveVisionRequest(root["request_id"].asString());
}

}  // namespace

void WSHandler::ws_msg_handle(const std::string& message, bool is_binary, Application* app) {
    if (!is_binary) {
        Json::Value root;
        Json::Reader reader;
        bool parsingSuccessful = reader.parse(message, root);
        if (!parsingSuccessful) {
            USER_LOG_WARN("Error parsing message: %s", reader.getFormattedErrorMessages().c_str());
            app->eventQueue_.Enqueue(static_cast<int>(AppEvent::fault_happen));
            return;
        }

        USER_LOG_INFO("Received JSON message: %s", message.c_str());
        const Json::Value type = root["type"];
        if (type.isString()) {
            std::string typeStr = type.asString();
            if (typeStr == "vad") {
                handle_vad_message(root, app);
            } else if (typeStr == "asr") {
                handle_asr_message(root, app);
            } else if (typeStr == "chat") {
                handle_chat_message(root, app);
            } else if (typeStr == "tts") {
                handle_tts_message(root, app);
            } else if (typeStr == "vision_result") {
                handle_vision_result_message(root, app);
            } else if (typeStr == "error") {
                if (!IsStaleRequest(root, app)) {
                    USER_LOG_ERROR("server error msg: %s", message.c_str());
                    app->eventQueue_.Enqueue(static_cast<int>(AppEvent::fault_happen));
                }
            } else {
                USER_LOG_WARN("Unknown type: %s", typeStr.c_str());
            }
        }

        if (root.isMember("function_call") && root["function_call"].isObject()) {
            handle_intent_message(root, app);
            app->IntentQueue_.Enqueue(root);
        }
        return;
    }

    if (!app->ShouldAcceptIncomingAudio()) {
        USER_LOG_WARN("Drop binary audio because current TTS owner is stale.");
        return;
    }

    if (app->get_first_audio_msg_received() == true) {
        app->set_first_audio_msg_received(false);
        app->eventQueue_.Enqueue(static_cast<int>(AppEvent::speaking_msg_received));
    }

    BinProtocolInfo protocol_info;
    std::vector<uint8_t> opus_data;
    std::vector<int16_t> pcm_data;

    if (app->audio_processor_.UnpackBinFrame(reinterpret_cast<const uint8_t*>(message.data()), message.size(), protocol_info, opus_data)) {
        if (protocol_info.version == app->get_ws_protocolVersion() && protocol_info.type == 0) {
            app->audio_processor_.decode(opus_data.data(), opus_data.size(), pcm_data);
            app->audio_processor_.addFrameToPlaybackQueue(pcm_data);
        } else {
            USER_LOG_WARN("Received frame with unexpected version or type");
        }
    } else {
        USER_LOG_WARN("Failed to unpack binary frame");
    }
}

void WSHandler::handle_vad_message(const Json::Value& root, Application* app) {
    const Json::Value state = root["state"];
    if (state.isString() && state.asString() == "no_speech") {
        app->eventQueue_.Enqueue(static_cast<int>(AppEvent::vad_no_speech));
    }
}

void WSHandler::handle_asr_message(const Json::Value& root, Application* app) {
    const Json::Value text = root["text"];
    if (text.isString()) {
        USER_LOG_INFO("Received ASR text: %s", text.asString().c_str());
    } else {
        USER_LOG_WARN("Invalid ASR text value.");
    }
    app->eventQueue_.Enqueue(static_cast<int>(AppEvent::asr_received));
}

void WSHandler::handle_tts_message(const Json::Value& root, Application* app) {
    if (IsStaleRequest(root, app)) {
        USER_LOG_WARN("Drop stale TTS message. request_id=%s", root["request_id"].asString().c_str());
        app->SetAcceptIncomingAudio(false);
        return;
    }

    const Json::Value state = root["state"];
    if (state.isString()) {
        const std::string stateStr = state.asString();
        if (stateStr == "start") {
            USER_LOG_INFO("Received TTS start.");
            app->SetAcceptIncomingAudio(true);
        } else if (stateStr == "interrupted") {
            USER_LOG_WARN("Received TTS interrupted.");
            app->InterruptTTS(root.get("reason", "remote_interrupt").asString());
        } else if (stateStr == "end") {
            USER_LOG_INFO("Received TTS end.");
            app->set_tts_completed(true);
            app->SetAcceptIncomingAudio(true);
            if (HasRequestId(root)) {
                app->ClearActiveVisionRequest(root["request_id"].asString());
            }
        }
    }
}

void WSHandler::handle_chat_message(const Json::Value& root, Application* app) {
    if (IsStaleRequest(root, app)) {
        USER_LOG_WARN("Drop stale chat message. request_id=%s", root["request_id"].asString().c_str());
        return;
    }

    const Json::Value dialogue = root["dialogue"];
    if (dialogue.isString() && dialogue.asString() == "end") {
        USER_LOG_INFO("Received dialogue end.");
        app->set_dialogue_completed(true);
        if (HasRequestId(root)) {
            app->ClearActiveVisionRequest(root["request_id"].asString());
        }
    }
}

void WSHandler::handle_vision_result_message(const Json::Value& root, Application* app) {
    if (IsStaleRequest(root, app)) {
        USER_LOG_WARN("Drop stale vision result. request_id=%s", root["request_id"].asString().c_str());
        return;
    }

    if (root.isMember("text") && root["text"].isString()) {
        USER_LOG_INFO("Vision result: %s", root["text"].asString().c_str());
        app->UpdateVisionContext(root.get("request_id", "").asString(),
                                 root.get("source", "unknown").asString(),
                                 root.get("prompt", "").asString(),
                                 root["text"].asString(),
                                 root.get("frame_seq", Json::UInt64(0)).asUInt64(),
                                 root.get("captured_ms", Json::Int64(0)).asInt64());
    }
}

void WSHandler::handle_intent_message(const Json::Value& root, Application* app) {
    const Json::Value function_call = root["function_call"];
    if (!function_call.isMember("name") || !function_call["name"].isString()) {
        USER_LOG_ERROR("Invalid function_call structure in JSON: %s", root.toStyledString().c_str());
        return;
    }

    std::string intent_name = function_call["name"].asString();
    if (intent_name == "look_at_environment") {
        USER_LOG_INFO("Cloud requested vision. Submitting fresh frame request...");
        if (!app->SubmitVisionQuestion("Please describe the current camera frame in Chinese.", "passive_voice")) {
            USER_LOG_WARN("No fresh camera frame, cannot submit vision question.");
        }
        return;
    }

    if (function_call.isMember("arguments") && function_call["arguments"].isObject()) {
        IntentHandler::HandleIntent(root);
    }
}
