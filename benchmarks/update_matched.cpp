#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>
#define DECLARE(name) extern "C" void* name##_create(); extern "C" void name##_update(void*); extern "C" void name##_verify(void*); extern "C" void name##_destroy(void*);
DECLARE(pre)
DECLARE(before)
DECLARE(after)
struct Engine {
    const char* name;
    void* db;
    void (*update)(void*);
    void (*verify)(void*);
    void (*destroy)(void*);
    std::vector<double> samples;
    ~Engine() { destroy(db); }
};
double median(std::vector<double> samples) { std::sort(samples.begin(), samples.end()); return samples[samples.size() / 2]; }
int main() {
    try {
        Engine engines[] = {
            {"pre_json_4d039c2fae", pre_create(), pre_update, pre_verify, pre_destroy, {}},
            {"before_84efa5763d", before_create(), before_update, before_verify, before_destroy, {}},
            {"after", after_create(), after_update, after_verify, after_destroy, {}}
        };
        for (auto& engine : engines) for (int i = 0; i < 5; ++i) engine.update(engine.db);
        for (int round = 0; round < 15; ++round) {
            // All six orders, sharing one process and compiler/runtime.
            static constexpr int orders[][3] = {{0,1,2},{2,1,0},{1,2,0},{0,2,1},{2,0,1},{1,0,2}};
            for (int index : orders[round % 6]) {
                auto& engine = engines[index];
                auto start = std::chrono::steady_clock::now();
                for (int repeat = 0; repeat < 20; ++repeat) engine.update(engine.db);
                engine.samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 20);
            }
        }
        std::cout << "engine,rows,median_us,min_us,max_us,paired_ratio_to_pre_json\n";
        for (const auto& engine : engines) {
            engine.verify(engine.db); std::vector<double> ratios;
            for (std::size_t i = 0; i < engine.samples.size(); ++i) ratios.push_back(engine.samples[i] / engines[0].samples[i]);
            std::cout << engine.name << ",100000," << median(engine.samples) << ','
                << *std::min_element(engine.samples.begin(), engine.samples.end()) << ','
                << *std::max_element(engine.samples.begin(), engine.samples.end()) << ',' << median(ratios) << '\n';
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
