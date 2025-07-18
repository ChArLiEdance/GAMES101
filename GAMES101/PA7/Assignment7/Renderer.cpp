//
// Created by goksu on 2/25/20.
//

#include <fstream>
#include <cstdio>
#include "Scene.hpp"
#include "Renderer.hpp"
#include <thread>
#include <mutex>
#include <vector>
#include <iostream>

inline float deg2rad(const float& deg) { return deg * M_PI / 180.0; }

const float EPSILON = 0.00001;

// The main render function. This where we iterate over all pixels in the image,
// generate primary rays and cast these rays into the scene. The content of the
// framebuffer is saved to a file.
void Renderer::Render(const Scene& scene)
{
    std::vector<Vector3f> framebuffer(scene.width * scene.height);
    float scale = tan(deg2rad(scene.fov * 0.5));
    float imageAspectRatio = scene.width / (float)scene.height;
    Vector3f eye_pos(278, 273, -800);
    int spp;
    std::cout << "Enter samples per pixel (spp) [default 256]: ";
    if (!(std::cin >> spp)) {
        spp = 256;
    }

    std::mutex progressMutex; // 用来保护 UpdateProgress
    int finishedRows = 0;
    int numThreads = std::thread::hardware_concurrency(); // CPU 核心数
    if (numThreads == 0) numThreads = 4; // 保险：如果返回0，就手动指定
    printf("[INFO] Using %d threads for rendering\n", numThreads);

    // 每个线程要执行的任务
    auto renderTask = [&](int startRow, int endRow){
        for (int j = startRow; j < endRow; ++j) {
            for (int i = 0; i < scene.width; ++i) {
                int m = j * scene.width + i;
                float x = (2 * (i + 0.5f) / (float)scene.width - 1) * imageAspectRatio * scale;
                float y = (1 - 2 * (j + 0.5f) / (float)scene.height) * scale;
                Vector3f dir = normalize(Vector3f(-x, y, 1));
                Vector3f color(0, 0, 0);
                for (int k = 0; k < spp; k++){
                    color += scene.castRay(Ray(eye_pos, dir), 0);
                }
                framebuffer[m] = color / (float)spp;
            }
            {
                std::lock_guard<std::mutex> lock(progressMutex);
                finishedRows++;
                UpdateProgress(finishedRows / (float)scene.height);
            }
        }
    };

    // 按行分块
    std::vector<std::thread> threads;
    int rowsPerThread = scene.height / numThreads;
    int start = 0;
    for (int t = 0; t < numThreads; ++t) {
        int end = (t == numThreads - 1) ? scene.height : start + rowsPerThread;
        threads.emplace_back(renderTask, start, end);
        start = end;
    }

    // 等待所有线程结束
    for (auto &th : threads) th.join();

    UpdateProgress(1.f);

    // 保存图片
    FILE* fp = fopen("binary.ppm", "wb");
    fprintf(fp, "P6\n%d %d\n255\n", scene.width, scene.height);
    for (size_t i = 0; i < framebuffer.size(); ++i) {
        unsigned char color[3];
        color[0] = (unsigned char)(255 * std::pow(clamp(0, 1, framebuffer[i].x), 0.6f));
        color[1] = (unsigned char)(255 * std::pow(clamp(0, 1, framebuffer[i].y), 0.6f));
        color[2] = (unsigned char)(255 * std::pow(clamp(0, 1, framebuffer[i].z), 0.6f));
        fwrite(color, 1, 3, fp);
    }
    fclose(fp);
}