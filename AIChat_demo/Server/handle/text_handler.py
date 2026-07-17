from service_manager import ServiceManager
from tools.logger import logger
from config.settings import global_settings
from tools.registry import global_registry


class TextHandler:
    def __init__(self, service_manager: ServiceManager):
        self.service_manager = service_manager

    async def handle_text_message(self, data: dict):
        if data.get('type') == 'hello':
            audio_params = data.get('audio_params', {})
            logger.info(f"Received hello message with audio params: {audio_params}")
            api_key = data.get('api_key', None)
            global_settings.Set_API_Key(api_key)

        elif data.get('type') == 'functions_register':
            functions = data.get('functions', [])
            if not isinstance(functions, list):
                logger.error("functions field must be a list")
                return {"type": "error", "message": "Invalid functions format"}
            self.handle_register_functions(functions)

        elif data.get('type') == 'describe_scene':
            base64_image = data.get('image')
            if not base64_image:
                logger.warning("Received describe_scene without image data")
                return

            request_id = data.get('request_id')
            source = data.get('source', 'unknown')
            prompt = data.get('prompt') or "请用中文简洁描述当前画面。"
            frame_seq = data.get('frame_seq')
            captured_ms = data.get('captured_ms')
            logger.info(
                f"Received describe_scene request_id={request_id}, "
                f"source={source}, frame_seq={frame_seq}, captured_ms={captured_ms}"
            )
            self.service_manager.tts_service.tts_set(
                on_data=self.service_manager._tts_on_data,
                on_complete=self.service_manager._tts_on_complete,
            )
            self.service_manager.task_manager.submit_task(
                self.service_manager.vision_start_task,
                base64_image,
                request_id,
                prompt,
                source,
                frame_seq or 0,
                captured_ms or 0,
            )

        elif data.get('type') == 'state':
            if data.get('state') == 'idle':
                self.service_manager.reset_services()
                logger.info("Client is idle, resetting services")

            elif data.get('state') == 'listening':
                self.service_manager.interrupt_active_tts("client_listening")
                self.service_manager.is_vad = False
                self.service_manager.has_active_speech = False
                self.service_manager.vad_service.reset()
                self.service_manager.asr_service.reset()
                self.service_manager.tts_service.tts_set(
                    on_data=self.service_manager._tts_on_data,
                    on_complete=self.service_manager._tts_on_complete,
                )

            elif data.get('state') == 'thinking':
                logger.info("Client is thinking")

            elif data.get('state') == 'speaking':
                logger.info("Client is speaking")

        else:
            logger.warning(f"Unknown JSON message type: {data.get('type')}")
            logger.info(f"Received unknown message: {data}")
            return {"type": "error", "message": "Unknown message type"}

    def handle_register_functions(self, functions: list):
        for func in functions:
            try:
                function_name = func.get('name')
                description = func.get('description', '')
                arguments = func.get('arguments', {})

                if not function_name or not isinstance(arguments, dict):
                    logger.error(f"Function registration failed, missing fields: {func}")
                    continue

                global_registry.register_function(
                    function_name=function_name,
                    description=description,
                    parameters=arguments,
                    impl=self._generic_function_callback,
                )
                logger.info(
                    f"Registered function: {function_name}, description={description}, arguments={arguments}"
                )

            except Exception as e:
                logger.error(f"Function registration error: {e}")

    def _generic_function_callback(self, function_name: str, *args, **kwargs):
        logger.info(f"Function '{function_name}' called with args={args}, kwargs={kwargs}")
        return "Function executed successfully"
