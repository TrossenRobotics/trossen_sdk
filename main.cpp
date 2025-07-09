#include "trossen_data_collection_sdk/arms_move.hpp"

int main() {
    trossen_data_collection_sdk::TrossenAIStationary greeter("Trossen AI Stationary");
    greeter.sleep_arms(); // Say hello to the user
    return 0;
}
