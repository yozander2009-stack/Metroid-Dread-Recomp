# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# shaderdec on its own: a cross build (cmake/shaderdec.cmake) builds it with this project for the machine it runs on
cmake_minimum_required(VERSION 3.25)
project(shaderdec LANGUAGES C)
add_executable(shaderdec shaderdec.c)
# strict ISO C99, as the main build's strict C
set_target_properties(shaderdec PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
