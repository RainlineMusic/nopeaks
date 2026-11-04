#include <exception>
#include <iostream>

void testDsp();
void testOptimizer();
void testSharedBus();
void testCoordinator();
void testAudioQuality();

int main()
{
    try {
        testDsp();
        std::cout << "DSP tests passed\n";
        testOptimizer();
        std::cout << "Optimizer tests passed\n";
        testSharedBus();
        std::cout << "Shared-memory transport tests passed\n";
        testCoordinator();
        std::cout << "Coordinated audio tests passed\n";
        testAudioQuality();
        std::cout << "Audio quality regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Test failure: unknown exception\n";
        return 1;
    }
}
