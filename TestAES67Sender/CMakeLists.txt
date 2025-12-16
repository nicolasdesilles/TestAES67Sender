project(TestAES67Sender VERSION 0.0.1)

set (TargetName ${PROJECT_NAME})

# Set C++17 standard (required by ravennakit)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

juce_add_gui_app(${TargetName} PRODUCT_NAME "TestAES67Sender")
juce_generate_juce_header(${TargetName})

target_sources(${TargetName} PRIVATE
        Source/Main.cpp
        Source/MainComponent.cpp
        Source/MainWindow.cpp
        Source/RavennaNodeManager.cpp)

target_compile_definitions(${TargetName} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${TargetName},JUCE_PROJECT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${TargetName},JUCE_VERSION>")

target_link_libraries(${TargetName} PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
        juce::juce_gui_basics
        shared_processing_code
        ravennakit)

# macOS: Simple ad-hoc code signing (no sandbox entitlements)
# This helps with macOS code signature requirements
if (APPLE)
    add_custom_command(TARGET ${TargetName} POST_BUILD
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:${TargetName}>"
        COMMENT "Ad-hoc code signing ${TargetName}.app..."
    )
endif()
