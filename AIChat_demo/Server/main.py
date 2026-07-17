import asyncio
import sys

from ws_server import WebSocketServer
from threads.tts_thread import TTSGenerateThread
from threads.audio_send_thread import AudioSendThread
from tools.logger import logger
from service_manager import ServiceManager

sys.path.append("..")


async def main():
    service_manager = ServiceManager()

    tts_generate_thread = TTSGenerateThread(service_manager)
    # tts_generate_thread.start()

    tts_send_thread = AudioSendThread(service_manager)
    tts_send_thread.start()

    server = WebSocketServer(host="0.0.0.0", port=8000, access_token="123456", service_manager=service_manager)
    try:
        await server.start_server()
    except KeyboardInterrupt:
        logger.info("Server is shutting down...")
    finally:
        service_manager.stop_event.set()
        service_manager.task_manager.shutdown()
        # tts_generate_thread.join()
        tts_send_thread.join()
        logger.info("Server closed.")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Program interrupted by user")
    finally:
        try:
            loop = asyncio.get_event_loop()
            if loop.is_running():
                loop.stop()
        except RuntimeError:
            pass
        logger.info("Event loop closed")
