/**
 * @file main.cpp
 *
 * @brief Main entry point for the SOMA application
 */

#include "trossen_sdk/trossen_sdk.hpp"

#include "soma/soma.hpp"

int main() {
  soma::SomaApp app;
  return(app.run());
}
