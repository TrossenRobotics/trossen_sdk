#include <torch/torch.h>
#include <iostream>

int main() {
    // Check if CUDA is available
    torch::Device device(torch::kCPU);
    if (torch::cuda::is_available()) {
        std::cout << "CUDA is available. Using GPU." << std::endl;
        device = torch::Device(torch::kCUDA);
    } else {
        std::cout << "CUDA is not available. Using CPU." << std::endl;
    }

    // Create two tensors on the chosen device
    torch::Tensor a = torch::rand({3, 3}, device);
    torch::Tensor b = torch::rand({3, 3}, device);

    std::cout << "Tensor A:\n" << a << "\n";
    std::cout << "Tensor B:\n" << b << "\n";

    // Matrix multiplication
    torch::Tensor c = torch::matmul(a, b);
    std::cout << "A * B:\n" << c << "\n";

    return 0;
}
