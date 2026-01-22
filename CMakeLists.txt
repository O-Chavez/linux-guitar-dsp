cmake_minimum_required(VERSION 3.16)

# Monorepo root: engine lives under engine/, UI under app/.
project(dsp_engine_v1_monorepo LANGUAGES CXX)

add_subdirectory(engine)
