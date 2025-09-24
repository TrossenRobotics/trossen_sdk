# Trossen Data SDK v2 Roadmap

This document outlines the planned features and improvements for the Trossen Data SDK v2.

## Goals

- Provide a flexible and extensible SDK for recording and playing back robot data
- Support multiple backends for data storage (e.g., LeRobot, MCAP)
- Enable easy integration with various data sources and sinks
- Ensure high performance and low latency for real-time, data intensive applications
- Provide instrumentation and metrics for performance monitoring

## Non-Goals

- Real-time data processing or analysis
- Complex data visualization tools
- Robotics control algorithms
- Machine learning model training or inference
- Hardware interfacing or drivers (other than simple OpenCV camera wrappers)

## Features

### Backends

- [ ] LeRobot Backend
- [ ] MCAP Backend
- [ ] Trossen Backend
- [ ] Basic Custom Backend
- [x] Mock/Null Backend

### Sinks

- [x] Image Sink
- [x] Joint State Sink

### Producers

- [ ] Image Source
- [ ] Joint State Source
- [ ] Session Metadata Source
- [ ] Calibration Source
- [ ] Mock Sources

## Improvements

- [ ] Synchronization of multiple data streams based on timestamps
- [ ] Conversion utilities between different data formats (e.g., MCAP->LeRobot)
- [ ] Calibration data handling and storage

## Documentation

- [ ] Comprehensive API documentation
- [ ] Tutorials and examples for common use cases
- [ ] Best practices for using the SDK effectively
- [ ] Contribution guidelines for developers
- [ ] Changelog and release notes

## Testing

- [ ] Unit tests for all major components
- [ ] Integration tests for backend and sink interactions
- [ ] Performance benchmarks for data throughput and latency
