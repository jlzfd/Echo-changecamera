#include <iostream>
#include <stdexcept>
#include <string>

#include "Application/Application.h"

// Example:
// ./build/AIChatClient 192.168.211.1 8000 123456 ./model/yolov5.rknn
// ./build/AIChatClient 192.168.211.1 8000 123456 ./model/yolov5.rknn ./model/face_detect.rknn
void print_usage(const char* progname) {
    std::cout << "Usage: " << progname
              << " <server_address> <port> <token> <yolov5_model_path> [face_model_path]"
              << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        const std::string address = argv[1];
        const int port = std::stoi(argv[2]);
        const std::string token = argv[3];
        const std::string model_path = argv[4];
        const std::string face_model_path = (argc >= 6) ? argv[5] : "";

        const std::string deviceId = "00:11:22:33:44:55";
        const std::string aliyun_api_key = "sk-898ad55d5c9346b2bfb13b13009cd266";
        const int protocolVersion = 2;
        const int sample_rate = 16000;
        const int channels = 1;
        const int frame_duration = 40;

        Application app(address, port, token, deviceId, aliyun_api_key, protocolVersion,
                        sample_rate, channels, frame_duration, model_path, face_model_path);

        std::cout << ">>> Running AI Chat Application..." << std::endl;
        app.Run();
        std::cout << ">>> Application stopped." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
