# ChinaWarlords + Potato Engine - CMake 建置配置

cmake_minimum_required(VERSION 3.15)
project(ChinaWarlords VERSION 1.0.0 LANGUAGES CXX)

# 設置 C++ 標準
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 設置輸出目錄
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 編譯選項
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# 安全強化旗標 (Security hardening)
# 可用 -DPOTATO_SECURITY_HARDENING=OFF 關閉
# ============================================================================
option(POTATO_SECURITY_HARDENING "Enable security hardening flags" ON)
if(POTATO_SECURITY_HARDENING)
    if(MSVC)
        # /sdl: SDL 安全檢查, /GS: 堆疊保護, /guard:cf: 控制流防護
        add_compile_options(/sdl /GS /guard:cf)
        # ASLR + DEP + 高位址熵 + CFG + CET 陰影堆疊
        add_link_options(/DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /CETCOMPAT)
    else()
        # 堆疊保護 + PIE (位置無關執行檔，啟用 ASLR)
        add_compile_options(-fstack-protector-strong -fstack-clash-protection -fPIE)
        if(WIN32)
            # MinGW: PE 格式加固。-pie 只適用執行檔——套到 SHARED/MODULE 會拉進
            # exe CRT 要求 WinMain（如 FakeCheat 測試 DLL）。
            add_link_options($<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-pie>)
            add_link_options(-Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va)
        else()
            # Linux/macOS: ELF 加固（-pie 同樣僅限執行檔）
            add_compile_definitions(_FORTIFY_SOURCE=2)
            add_link_options($<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-pie>)
            add_link_options(-Wl,-z,relro,-z,now -Wl,-z,noexecstack)
        endif()
    endif()
endif()

# GUI 依賴（glfw/glad/OpenGL）——headless 建置可用 -DPOTATO_BUILD_GUI=OFF
# 略過窗口化執行檔與 FetchContent 回退路徑。vendored 原生庫
# （external/glfw、external/glad_gen、external/imgui）無網路/git 需求、
# 編譯也不需要 GL context，故不受此開關影響恆編——headless 的
# RenderingServer（Dummy rasterizer）與 CPU 容器（Mesh/Texture）仍
# 需它們的符號做連結期解析。
option(POTATO_BUILD_GUI "Build windowed GL executables and allow glfw FetchContent fallback" ON)

# potato_test_* / CTest 註冊——關閉可加速純庫建置（CI 預設 ON 並跑 ctest）
option(POTATO_TESTS "Build potato_test_* targets and register them with CTest" ON)
option(POTATO_BENCHMARKS "Build opt-in performance measurement tools" OFF)

# glfw/glad 皆為 C 源碼——頂層 project() 僅啟用 CXX，此處按需啟用
enable_language(C)

# GLFW - 窗口和輸入管理
# vendored 副本優先（離線可建置）；不存在時才走 FetchContent——
# 該回退維持在 GUI 開關內，保住 OFF 語意（無 git/網路環境）。
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# glfw 3.4 turns Wayland on by default on UNIX — we only ship X11,
# and CI shouldn't need wayland-protocols/xkbcommon-wayland pieces.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
if(EXISTS ${CMAKE_SOURCE_DIR}/external/glfw/CMakeLists.txt)
    add_subdirectory(external/glfw)
elseif(POTATO_BUILD_GUI)
    include(FetchContent)
    FetchContent_Declare(
        glfw
        URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
        URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(glfw)
endif()

# GLAD - OpenGL 加載器（external/glad_gen 預生成——glad2 的
# FetchContent 需要 git+python；直接編譯 vendored 產物，
# 涵蓋 GL 3.3 core，符合引擎 Rendering/ 的 context 需求）
add_library(glad STATIC external/glad_gen/src/glad.c)
target_include_directories(glad PUBLIC
    ${CMAKE_SOURCE_DIR}/external/glad_gen/include)

# Dear ImGui — vendored (external/imgui). The GL/GLFW backends use
# imgui's embedded GL loader, so imgui itself never touches glad.
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/imgui/backends)
target_link_libraries(imgui PUBLIC glfw)

if(POTATO_BUILD_GUI)
# 查找依賴（窗口化執行檔才需要系統 GL）
find_package(OpenGL REQUIRED)
endif()

# OpenAL - 音頻庫 (可選)
# include(FetchContent)
# FetchContent_Declare(
#     openal
#     GIT_REPOSITORY https://github.com/kcat/openal-soft.git
#     GIT_TAG 1.23.1
# )
# FetchContent_MakeAvailable(openal)

# 包含目錄
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Core
    ${CMAKE_SOURCE_DIR}/Rendering
    ${CMAKE_SOURCE_DIR}/Physics
    ${CMAKE_SOURCE_DIR}/Audio
    ${CMAKE_SOURCE_DIR}/Input
    ${CMAKE_SOURCE_DIR}/Resources
    ${CMAKE_SOURCE_DIR}/ECS
    ${CMAKE_SOURCE_DIR}/GameObject
    ${CMAKE_SOURCE_DIR}/Events
    ${CMAKE_SOURCE_DIR}/FileSystem
    ${CMAKE_SOURCE_DIR}/Logging
    ${CMAKE_SOURCE_DIR}/MathUtils
    ${CMAKE_SOURCE_DIR}/Memory
    ${CMAKE_SOURCE_DIR}/Platform
    ${CMAKE_SOURCE_DIR}/Scene
    ${CMAKE_SOURCE_DIR}/Serialization
    ${CMAKE_SOURCE_DIR}/Time
    ${CMAKE_SOURCE_DIR}/AI
    ${CMAKE_SOURCE_DIR}/GUI
    ${CMAKE_SOURCE_DIR}/Security
    ${OPENGL_INCLUDE_DIRS}
)

# Project files - Gradually enable subsystems
file(GLOB_RECURSE ENGINE_SOURCES 
    "Core/*.cpp"
    "Security/*.cpp"      # 安全模組 - 自包含，無引擎依賴
    # "Events/*.cpp"
    # "FileSystem/*.cpp"      # Temporarily disabled - implementation file has structural issues
    # "Logging/*.cpp"      # Temporarily disabled - depends on CoreTypes
    # "Memory/*.cpp"      # Temporarily disabled - implementation file has structural issues
    # "Platform/*.cpp"      # Temporarily disabled - depends on GLFW
    # "Scene/*.cpp"      # Temporarily disabled - depends on Matrix4/Quaternion
    # "Serialization/*.cpp"      # Temporarily disabled
    # "Time/*.cpp"      # Temporarily disabled - implementation file has structural issues
    # "Rendering/*.cpp"      # Temporarily disabled - depends on Math
    # "Physics/*.cpp"      # Temporarily disabled - depends on Math
    # "Audio/*.cpp"      # Temporarily disabled - depends on Math
    # "Input/*.cpp"      # Temporarily disabled - depends on Math
    # "Resources/*.cpp"      # Temporarily disabled - depends on Math
    # "ECS/*.cpp"      # Temporarily disabled - depends on Math
    # "GameObject/*.cpp"      # Temporarily disabled - depends on Math
    # "AI/*.cpp"      # Temporarily disabled - encoding issues need fixing
    # "GUI/*.cpp"      # Temporarily disabled
    # "MathUtils/*.cpp"      # Temporarily disabled
)

file(GLOB_RECURSE ENGINE_HEADERS
    "Core/*.h"
    "Rendering/*.h"
    "Physics/*.h"
    "Audio/*.h"
    "Input/*.h"
    "Resources/*.h"
    "ECS/*.h"
    "GameObject/*.h"
    "Events/*.h"
    "FileSystem/*.h"
    "Logging/*.h"
    "Memory/*.h"
    "Platform/*.h"
    "Scene/*.h"
    "Serialization/*.h"
    "Time/*.h"
    "AI/*.h"
    "GUI/*.h"
    "MathUtils/*.h"
)

# 創建引擎庫
add_library(PotatoEngine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})

target_include_directories(PotatoEngine PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Core
    ${CMAKE_SOURCE_DIR}/Core/Interfaces
    ${CMAKE_SOURCE_DIR}/MathUtils
)

target_link_libraries(PotatoEngine
    ${OPENGL_LIBRARIES}
    # glfw
    # glad
    ${CMAKE_DL_LIBS}
)

# Windows: psapi.lib（Security 模組的模組枚舉用；MSVC 由 #pragma comment 處理，MinGW 需顯式連結）
if(WIN32 AND NOT MSVC)
    target_link_libraries(PotatoEngine psapi wintrust bcrypt)
endif()

target_compile_definitions(PotatoEngine PRIVATE POTATO_ENGINE_VERSION="${PROJECT_VERSION}")
target_compile_definitions(PotatoEngine PRIVATE _USE_MATH_DEFINES)

# 示例程序
# add_executable(SimpleExample Examples/SimpleExample.cpp)
# target_link_libraries(SimpleExample PotatoEngine)

# 最小測試程序
add_executable(MinimalTest Examples/MinimalTest.cpp)
target_include_directories(MinimalTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(MinimalTest)

# 引擎示例程序
add_executable(EngineExample Examples/EngineExample.cpp)
target_include_directories(EngineExample PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(EngineExample PotatoEngine)

# AI 演示程序（編碼兼容版本）
add_executable(AIDemo Examples/AIDemo.cpp)
target_link_libraries(AIDemo)

# 優化 AI 演示程序（性能優化版本）
add_executable(OptimizedAIDemo Examples/OptimizedAIDemo.cpp)
target_link_libraries(OptimizedAIDemo)

# AI 測試套件（完整測試驗證）
add_executable(AITestSuite Examples/AITestSuite.cpp)
target_link_libraries(AITestSuite)

# AI 增強模塊 - 神經網絡
add_library(NeuralNetwork STATIC AI/NeuralNetwork.cpp)
target_include_directories(NeuralNetwork PUBLIC ${CMAKE_SOURCE_DIR}/AI)

# AI 增強模塊 - 強化學習
add_library(ReinforcementLearning STATIC
    AI/ReinforcementLearning.cpp
    AI/QuantumSampler.cpp
)
target_include_directories(ReinforcementLearning PUBLIC
    ${CMAKE_SOURCE_DIR}/AI
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(ReinforcementLearning NeuralNetwork PotatoQuantum)

# AI 增強模塊 - 自然語言處理
add_library(NaturalLanguageProcessing STATIC AI/NaturalLanguageProcessing.cpp)
# Quantum 引擎模組 - 態向量模擬器 (Epic 13.1)
# FP lib — Campaign/Game 層專用，PotatoGameplay 不得連結（整數確定性）
add_library(PotatoQuantum STATIC
    Quantum/QubitRegister.cpp
    Quantum/Qudit.cpp
)
target_include_directories(PotatoQuantum PUBLIC ${CMAKE_SOURCE_DIR})

target_include_directories(NaturalLanguageProcessing PUBLIC ${CMAKE_SOURCE_DIR}/AI)

# AI Agent 系統
add_library(AIAgentSystem STATIC AI/AIAgentSystem.cpp)
target_include_directories(AIAgentSystem PUBLIC ${CMAKE_SOURCE_DIR}/AI)

# AI 增強模塊 - LLM 集成（HttpClient.cpp 提供 WinHTTP 傳輸層；
# MSVC 由 #pragma comment 連 winhttp，MinGW 需顯式連結）
add_library(LLMIntegration STATIC
    AI/LLMIntegration.cpp
    Networking/HttpClient.cpp)
target_include_directories(LLMIntegration PUBLIC ${CMAKE_SOURCE_DIR}/AI)
if(WIN32 AND NOT MSVC)
    target_link_libraries(LLMIntegration winhttp)
endif()

# AI 增強模塊 - RAG 系統
add_library(RAGSystem STATIC AI/RAGSystem.cpp)
target_include_directories(RAGSystem PUBLIC ${CMAKE_SOURCE_DIR}/AI)
target_link_libraries(RAGSystem LLMIntegration)

# AI 增強模塊 - Agent 鏈
add_library(AgentChain STATIC AI/AgentChain.cpp)
target_include_directories(AgentChain PUBLIC ${CMAKE_SOURCE_DIR}/AI)
target_link_libraries(AgentChain LLMIntegration)

# AI 增強模塊 - 工具框架
add_library(ToolFramework STATIC AI/ToolFramework.cpp)
target_include_directories(ToolFramework PUBLIC ${CMAKE_SOURCE_DIR}/AI)
target_link_libraries(ToolFramework LLMIntegration)

# AI 增強模塊 - 智能開發系統
add_library(IntelligentDevelopmentSystem STATIC AI/IntelligentDevelopmentSystem.cpp)
target_include_directories(IntelligentDevelopmentSystem PUBLIC ${CMAKE_SOURCE_DIR}/AI)
target_link_libraries(IntelligentDevelopmentSystem LLMIntegration RAGSystem AgentChain ToolFramework AIAgentSystem NeuralNetwork NaturalLanguageProcessing)

# 渲染模組（含 RenderingServer——Godot 式 RID server/culler/rasterizer
# 分層，自上游 PotatoEngine 同步）。全部源碼上游驗證可編；GL 段僅需
# vendored 標頭與函式庫（glad_gen/glfw/imgui 恆編），執行期才要 GL
# context——headless 測試走 Dummy rasterizer 路徑。
file(GLOB RENDERING_SOURCES CONFIGURE_DEPENDS "Rendering/*.cpp")
add_library(PotatoRendering STATIC
    ${RENDERING_SOURCES}
    Logging/Logger.cpp
    Logging/Log.cpp
    Scene/SceneNode.cpp
    Platform/GLFWSharedContext.cpp
    Platform/PlatformSystem.cpp
    Input/InputManager.cpp
)
target_include_directories(PotatoRendering PUBLIC ${CMAKE_SOURCE_DIR}
    PRIVATE
    ${CMAKE_SOURCE_DIR}/external/glad_gen/include
    ${CMAKE_SOURCE_DIR}/external/glfw/include
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/tinygltf
)
target_link_libraries(PotatoRendering PUBLIC
    NeuralNetwork glad glfw imgui ${OPENGL_LIBRARIES})

# AI 增強模塊 - Agent 繪圖工具（RenderingServer ↔ ToolFramework 橋接；
# Dummy rasterizer 下 agent 可無 GL 組裝並驗證場景）
add_library(RenderAgentTools STATIC AI/RenderAgentTools.cpp)
target_include_directories(RenderAgentTools PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(RenderAgentTools ToolFramework PotatoRendering NeuralNetwork)

# AI 增強模塊 - Agent 解析數論工具橋接（MathUtils/RiemannZeta ↔
# ToolFramework；全 headless 純 CPU——ζ/L 求值、零點掃描、RH 等價判據）
add_library(ZetaAgentTools STATIC AI/ZetaAgentTools.cpp)
target_include_directories(ZetaAgentTools PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(ZetaAgentTools ToolFramework)

# AI 增強模塊 - LLM function-calling 橋（ToolDefinition schema、
# ToolCall.arguments JSON 解析、派發——LLM 直接驅動 render_*/zeta_*）
add_library(LLMToolBridge STATIC AI/LLMToolBridge.cpp)
target_include_directories(LLMToolBridge PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(LLMToolBridge LLMIntegration ToolFramework)

if(POTATO_TESTS)
# RenderingServer——RID 代際、canvas 剔除/z-order、scene cull、
# dummy rasterizer 指令流、agent 工具端到端（全 headless）
add_executable(RenderingServerTest Examples/RenderingServerTest.cpp)
target_include_directories(RenderingServerTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(RenderingServerTest PotatoRendering RenderAgentTools ToolFramework NeuralNetwork)
add_test(NAME RenderingServerTest COMMAND RenderingServerTest)

# 黎曼 ζ 探索——ZetaEM/RSZ 驗證 + 臨界帶地形圖（RiemannZeta.h 共用
# 數值核心；ImageCodec 由 PotatoRendering 提供）
add_executable(RiemannExplorerTest Examples/RiemannExplorerTest.cpp)
target_include_directories(RiemannExplorerTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(RiemannExplorerTest PotatoRendering)
add_test(NAME RiemannExplorerTest COMMAND RiemannExplorerTest)

add_executable(RiemannExplorer2Test Examples/RiemannExplorer2Test.cpp)
target_include_directories(RiemannExplorer2Test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(RiemannExplorer2Test PotatoRendering)
add_test(NAME RiemannExplorer2Test COMMAND RiemannExplorer2Test)

# ζ agent 工具端到端（headless——tool 註冊/參數校驗/求值管線）
add_executable(ZetaAgentToolsTest Examples/ZetaAgentToolsTest.cpp)
target_include_directories(ZetaAgentToolsTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ZetaAgentToolsTest ZetaAgentTools ToolFramework)
add_test(NAME ZetaAgentToolsTest COMMAND ZetaAgentToolsTest)

# Riemann 第三/四波驗證（headless——MathUtils/RiemannZeta 數值核）
add_executable(RiemannExplorer3Test Examples/RiemannExplorer3Test.cpp)
target_include_directories(RiemannExplorer3Test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(RiemannExplorer3Test PotatoRendering)
add_test(NAME RiemannExplorer3Test COMMAND RiemannExplorer3Test)

add_executable(RiemannExplorer4Test Examples/RiemannExplorer4Test.cpp)
target_include_directories(RiemannExplorer4Test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(RiemannExplorer4Test PotatoRendering)
add_test(NAME RiemannExplorer4Test COMMAND RiemannExplorer4Test)

# LLM function-calling ↔ ToolRegistry 橋測試（headless）
add_executable(LLMToolBridgeTest Examples/LLMToolBridgeTest.cpp)
target_include_directories(LLMToolBridgeTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(LLMToolBridgeTest LLMToolBridge ZetaAgentTools ToolFramework LLMIntegration)
add_test(NAME LLMToolBridgeTest COMMAND LLMToolBridgeTest)
endif() # POTATO_TESTS

if(POTATO_BENCHMARKS)
    add_executable(potato_benchmark_simulation_lod
        Examples/MingGoRTS/potato_benchmark_simulation_lod.cpp)
    target_include_directories(potato_benchmark_simulation_lod PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(potato_benchmark_simulation_lod PotatoGameplay)
endif()

# MingGoRTS IDE 智能建議系統
add_library(IntelligentSuggestion STATIC MingGoRTS_IDE/IntelligentSuggestion.cpp)
target_include_directories(IntelligentSuggestion PUBLIC 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/MingGoRTS_IDE
)

# AI 增強演示程序
add_executable(AIEnhancedDemo Examples/AIEnhancedDemo.cpp)
target_include_directories(AIEnhancedDemo PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(AIEnhancedDemo NeuralNetwork ReinforcementLearning NaturalLanguageProcessing)

# 終極 AI Agent 演示程序
add_executable(UltimateAIAgentDemo Examples/UltimateAIAgentDemo.cpp)
target_include_directories(UltimateAIAgentDemo PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(UltimateAIAgentDemo AIAgentSystem LLMIntegration RAGSystem AgentChain ToolFramework NeuralNetwork NaturalLanguageProcessing)

# 智能開發系統演示程序
add_executable(IntelligentDevelopmentDemo Examples/IntelligentDevelopmentDemo.cpp)
target_include_directories(IntelligentDevelopmentDemo PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(IntelligentDevelopmentDemo IntelligentDevelopmentSystem LLMIntegration AIAgentSystem)

# MingGoRTS IDE（AI Agent 整合 IDE - 命令行版本）
add_executable(MingGoRTS_IDE_CLI MingGoRTS_IDE/MingGoRTS_IDE_Main.cpp MingGoRTS_IDE/IDECore.cpp MingGoRTS_IDE/AIIntegration.cpp)
target_include_directories(MingGoRTS_IDE_CLI PRIVATE ${CMAKE_SOURCE_DIR})

# MingGoRTS IDE（GUI 版本）
if(POTATO_BUILD_GUI)
add_executable(MingGoRTS_IDE_GUI
    MingGoRTS_IDE/GUI/Main.cpp
    MingGoRTS_IDE/GUI/IDEGUI.cpp
    MingGoRTS_IDE/GUI/IDEGUI.h
    MingGoRTS_IDE/GUI/I18N.cpp
    MingGoRTS_IDE/GUI/I18N.h
    MingGoRTS_IDE/IDECore.cpp
    MingGoRTS_IDE/AIIntegration.cpp
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(MingGoRTS_IDE_GUI PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/imgui/backends
)
target_link_libraries(MingGoRTS_IDE_GUI glfw ${OPENGL_LIBRARIES}
    IntelligentDevelopmentSystem
    IntelligentSuggestion
    LLMIntegration
    RAGSystem
    AgentChain
    ToolFramework
    AIAgentSystem
    NeuralNetwork
    NaturalLanguageProcessing
)
endif() # POTATO_BUILD_GUI

# 數學測試程序
add_executable(MathTest Examples/MathTest.cpp)
target_include_directories(MathTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(MathTest)

# 安全模組紅隊測試（對抗性驗證 Security/ 的偵測能力）
add_executable(SecurityRedTeamTest Examples/SecurityRedTeamTest.cpp)
target_include_directories(SecurityRedTeamTest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(SecurityRedTeamTest PotatoEngine)

# 紅隊測試用的無害 DLL（模擬被注入的作弊模組）
add_library(FakeCheatModule SHARED Examples/FakeCheatModule.cpp)
set_target_properties(FakeCheatModule PROPERTIES OUTPUT_NAME "FakeCheat" PREFIX "")

# 測試程序 (暫時禁用 - 依賴 PotatoEngine)
# add_executable(FileSystemTest Examples/FileSystemTest.cpp)
# target_link_libraries(FileSystemTest PotatoEngine)

# add_executable(LoggerTest Examples/LoggerTest.cpp)
# target_link_libraries(LoggerTest PotatoEngine)

# add_executable(MemoryTest Examples/MemoryTest.cpp)
# target_link_libraries(MemoryTest PotatoEngine)

# add_executable(PlatformTest Examples/PlatformTest.cpp)
# target_link_libraries(PlatformTest PotatoEngine)

# AI Agent GUI 示例 (暫時禁用)
# add_executable(AIAgentGUIExample Examples/AIAgentGUIExample.cpp)
# target_link_libraries(AIAgentGUIExample PotatoEngine)

# ==== ChinaWarlords 遊戲層（民國史詩，中文名暫定） ====
# Gameplay：純模擬函式庫——headless、無 Rendering/GUI/Platform-GL 依賴
# （禁入清單由 scripts/CheckGameplayDeps.ps1 檢查）
file(GLOB_RECURSE GAMEPLAY_SOURCES CONFIGURE_DEPENDS "Gameplay/*.cpp")
file(GLOB_RECURSE GAMEPLAY_HEADERS CONFIGURE_DEPENDS "Gameplay/*.h")
add_library(PotatoGameplay STATIC ${GAMEPLAY_SOURCES} ${GAMEPLAY_HEADERS})
target_include_directories(PotatoGameplay PUBLIC ${CMAKE_SOURCE_DIR})
# 注意：暫不連結 PotatoEngine——其唯二啟用實作（Security/*.cpp）在 MinGW 下
# 無法編譯（MSVC 專用寫法）；leaf 模組僅以標頭形式提供。待引擎修復或拆出
# 獨立 leaf target 後再恢復連結。

if(POTATO_TESTS)
# Headless 煙霧測試：固定種子決定性 tick
add_executable(potato_test_gameplay Examples/potato_test_gameplay.cpp)
target_include_directories(potato_test_gameplay PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_gameplay PotatoGameplay)

# JsonValue DOM parser + potato.<name>/<ver> schema gate 測試
add_executable(potato_test_json Examples/potato_test_json.cpp)
target_include_directories(potato_test_json PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_json PotatoGameplay)

# BattleMap region/node-graph model + potato.map/1 loader 測試
add_executable(potato_test_map Examples/potato_test_map.cpp)
target_include_directories(potato_test_map PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_map PotatoGameplay)

# Squad model + potato.squad/1 templates + cohesion/rout state table 測試
add_executable(potato_test_squad Examples/potato_test_squad.cpp)
target_include_directories(potato_test_squad PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_squad PotatoGameplay)

# TODO: POTATO_TESTS 清單為規劃項——目前以 CTest 直接註冊遊戲層測試
enable_testing()
add_test(NAME potato_test_gameplay COMMAND potato_test_gameplay)
add_executable(potato_test_simulation_lod
    Examples/MingGoRTS/potato_test_simulation_lod.cpp)
target_include_directories(potato_test_simulation_lod PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_simulation_lod PotatoGameplay)
add_test(NAME potato_test_simulation_lod COMMAND potato_test_simulation_lod)
add_test(NAME potato_test_json COMMAND potato_test_json)
add_test(NAME potato_test_map COMMAND potato_test_map)
add_test(NAME potato_test_squad COMMAND potato_test_squad)

# Doctrine interpreter + potato.doctrine_cards/1 library 測試
add_executable(potato_test_doctrine Examples/potato_test_doctrine.cpp)
target_include_directories(potato_test_doctrine PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_doctrine PotatoGameplay)
add_test(NAME potato_test_doctrine COMMAND potato_test_doctrine)

# BattleController 三拍迴圈測試
add_executable(potato_test_battle Examples/potato_test_battle.cpp)
target_include_directories(potato_test_battle PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_battle PotatoGameplay)
add_test(NAME potato_test_battle COMMAND potato_test_battle)

# CP interventions 測試
add_executable(potato_test_command Examples/potato_test_command.cpp)
target_include_directories(potato_test_command PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_command PotatoGameplay)
add_test(NAME potato_test_command COMMAND potato_test_command)

# QuantumFog 測試
add_executable(potato_test_fog Examples/potato_test_fog.cpp)
target_include_directories(potato_test_fog PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_fog PotatoGameplay)
add_test(NAME potato_test_fog COMMAND potato_test_fog)

# BattlePlan arrows 測試
add_executable(potato_test_plan Examples/potato_test_plan.cpp)
target_include_directories(potato_test_plan PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_plan PotatoGameplay)
add_test(NAME potato_test_plan COMMAND potato_test_plan)

# Symmetric AI 測試
add_executable(potato_test_ai Examples/potato_test_ai.cpp)
target_include_directories(potato_test_ai PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_ai PotatoGameplay)
add_test(NAME potato_test_ai COMMAND potato_test_ai)

# BattleRecorder / replay verifier 測試
add_executable(potato_test_record Examples/potato_test_record.cpp)
target_include_directories(potato_test_record PRIVATE ${CMAKE_SOURCE_DIR})
# Quantum module baseline (13.1) — engine leaf, NOT PotatoGameplay
add_executable(potato_test_quantum Examples/potato_test_quantum.cpp)
target_include_directories(potato_test_quantum PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_quantum PotatoQuantum)
add_test(NAME potato_test_quantum COMMAND potato_test_quantum)

# Story 13.3 — QuantumSampler + RL agent quantum-sampling opt-in
add_executable(potato_test_quantum_ai Examples/potato_test_quantum_ai.cpp)
target_include_directories(potato_test_quantum_ai PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_quantum_ai ReinforcementLearning)
add_test(NAME potato_test_quantum_ai COMMAND potato_test_quantum_ai)

target_link_libraries(potato_test_record PotatoGameplay)
add_test(NAME potato_test_record COMMAND potato_test_record)

# Story 1.12 — win evaluation (BattleResult, stalemate draw)
add_executable(potato_test_eval Examples/potato_test_eval.cpp)
target_include_directories(potato_test_eval PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_eval PotatoGameplay)
add_test(NAME potato_test_eval COMMAND potato_test_eval)

# Story 4.1 — GovernanceField battle events
add_executable(potato_test_governance Examples/potato_test_governance.cpp)
target_include_directories(potato_test_governance PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_governance PotatoGameplay)
add_test(NAME potato_test_governance COMMAND potato_test_governance)
endif() # POTATO_TESTS

# Replay-verifier CLI: potato_verify <record.json> -> VERIFY OK/FAIL
add_executable(potato_verify Examples/potato_verify.cpp)
target_include_directories(potato_verify PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_verify PotatoGameplay)

# ==== Campaign 層 ====
# Campaign：持久化層——headless、依賴方向 Gameplay <- Campaign（禁入
# 清單同樣由 scripts/CheckGameplayDeps.ps1 檢查，外加 Game/）
file(GLOB_RECURSE CAMPAIGN_SOURCES CONFIGURE_DEPENDS "Campaign/*.cpp")
file(GLOB_RECURSE CAMPAIGN_HEADERS CONFIGURE_DEPENDS "Campaign/*.h")
add_library(PotatoCampaign STATIC ${CAMPAIGN_SOURCES} ${CAMPAIGN_HEADERS})
target_include_directories(PotatoCampaign PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(PotatoCampaign PUBLIC PotatoGameplay)
# Story 13.2 — QuantumOracle adapter: PRIVATE so Quantum/ headers
# never reach Campaign consumers; symbols still resolve at link.
target_link_libraries(PotatoCampaign PRIVATE PotatoQuantum)

if(POTATO_TESTS)
# Auction-house escrow, proxy settlement, recruit award, and persistence.
add_executable(potato_test_auction
    Examples/MingGoRTS/potato_test_auction.cpp)
target_include_directories(potato_test_auction PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(potato_test_auction PRIVATE
    POTATO_AUCTION_DIR="${CMAKE_SOURCE_DIR}/assets/auctions")
target_link_libraries(potato_test_auction PotatoCampaign)
add_test(NAME potato_test_auction COMMAND potato_test_auction)

# Story 2.1 — campaign-account double-entry ledger
add_executable(potato_test_ledger Examples/potato_test_ledger.cpp)
target_include_directories(potato_test_ledger PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_ledger PotatoCampaign)
add_test(NAME potato_test_ledger COMMAND potato_test_ledger)

# Story 3.1 — CampaignState facade (potato.campaign/1)
add_executable(potato_test_campaign Examples/potato_test_campaign.cpp)
target_include_directories(potato_test_campaign PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_campaign PotatoCampaign)
add_test(NAME potato_test_campaign COMMAND potato_test_campaign)

# Story 5.1 — myth infiltration state machine (Gameplay MythField +
# Campaign MythState persistence)
add_executable(potato_test_myth Examples/potato_test_myth.cpp)
target_include_directories(potato_test_myth PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_myth PotatoCampaign)
add_test(NAME potato_test_myth COMMAND potato_test_myth)

# Story 10.1 — Bencao codex entry registry (potato.bencao/1)
add_executable(potato_test_bencao Examples/potato_test_bencao.cpp)
target_include_directories(potato_test_bencao PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_bencao PotatoCampaign)
add_test(NAME potato_test_bencao COMMAND potato_test_bencao)
# Game-only codex frontispiece attribution and adapted taxonomy.
add_executable(potato_test_bencao_frontispiece
               Examples/MingGoRTS/potato_test_bencao_frontispiece.cpp)
target_include_directories(potato_test_bencao_frontispiece
                           PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_bencao_frontispiece PotatoCampaign)
add_test(NAME potato_test_bencao_frontispiece
         COMMAND potato_test_bencao_frontispiece)
# Story 13.2 — QuantumOracle adapter (FP boundary: ints only out)
add_executable(potato_test_oracle Examples/potato_test_oracle.cpp)
target_include_directories(potato_test_oracle PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_oracle PotatoCampaign)
add_test(NAME potato_test_oracle COMMAND potato_test_oracle)


# Story 10.6 — launch content set (assets/bencao/*.json): registry
# invariants over the shipped catalog, not fixture data
add_executable(potato_test_bencao_launch
               Examples/potato_test_bencao_launch.cpp)
target_include_directories(potato_test_bencao_launch
                           PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(potato_test_bencao_launch PRIVATE
    POTATO_BENCAO_DIR="${CMAKE_SOURCE_DIR}/assets/bencao")
target_link_libraries(potato_test_bencao_launch PotatoCampaign)
add_test(NAME potato_test_bencao_launch
         COMMAND potato_test_bencao_launch)

# Story 11.1 — mint-claim schema + outbox (potato.mintclaim/1)
add_executable(potato_test_mintclaim Examples/potato_test_mintclaim.cpp)
target_include_directories(potato_test_mintclaim PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_mintclaim PotatoCampaign)
add_test(NAME potato_test_mintclaim COMMAND potato_test_mintclaim)

# Story 12.1 — world map schema + registry (potato.world/1)
add_executable(potato_test_world Examples/potato_test_world.cpp)
target_include_directories(potato_test_world PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_world PotatoCampaign)
add_test(NAME potato_test_world COMMAND potato_test_world)

# Epic 17 — Campaign world-agent persistence and LOD policy bridge.
add_executable(potato_test_world_agents
    Examples/MingGoRTS/potato_test_world_agents.cpp)
target_include_directories(potato_test_world_agents PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_world_agents PotatoCampaign)
add_test(NAME potato_test_world_agents COMMAND potato_test_world_agents)

# Story 12.4 — character model + creation (potato.character/1)
add_executable(potato_test_character Examples/potato_test_character.cpp)
target_include_directories(potato_test_character PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_character PotatoCampaign)
add_test(NAME potato_test_character COMMAND potato_test_character)

# Story 12.5 — encounter pipeline (potato.encounter/1): world
# position -> three-beat battle -> world writeback + gated claims
add_executable(potato_test_encounter Examples/potato_test_encounter.cpp)
target_include_directories(potato_test_encounter PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(potato_test_encounter PRIVATE
    POTATO_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
target_link_libraries(potato_test_encounter PotatoCampaign)
add_test(NAME potato_test_encounter COMMAND potato_test_encounter)

# Story 6.5 — narrative pack registry (assets/narrative/*.json)
add_executable(potato_test_narrative Examples/potato_test_narrative.cpp)
target_include_directories(potato_test_narrative PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_narrative PotatoCampaign)
add_test(NAME potato_test_narrative COMMAND potato_test_narrative)
endif() # POTATO_TESTS

# Story 11.4 — chain/outbox exporter: fixture producer for the
# external relayer (Tools/MintRelayer/) and the demo path.
add_executable(potato_export_chain Examples/potato_export_chain.cpp)
target_include_directories(potato_export_chain PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_export_chain PotatoCampaign)

# ==== Game 層 ====
# Game：表示層 —— 依賴方向 Gameplay <- Campaign <- Game。
# 注意：Game/Render 核心是 headless 軟體光柵（不依賴 GL），
# 未來的 GL presenter 才把視窗依賴掛上來。
# Game/Shell 是 GL presenter —— glfw/glad/imgui 依賴，永遠不進
# PotatoGame（無論 GUI 開關）；它由 potato_game 執行檔單獨編譯，
# 讓 PotatoGame 在任何建置下都保持 headless。
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "Game/*.cpp")
file(GLOB_RECURSE GAME_HEADERS CONFIGURE_DEPENDS "Game/*.h")
list(FILTER GAME_SOURCES EXCLUDE REGEX "Game/Shell/")
add_library(PotatoGame STATIC ${GAME_SOURCES} ${GAME_HEADERS})
target_include_directories(PotatoGame PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(PotatoGame PUBLIC PotatoCampaign)

if(POTATO_TESTS)
# Story 8.1 — sprite atlas + software map rendering
add_executable(potato_test_render Examples/potato_test_render.cpp)
target_include_directories(potato_test_render PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_render PotatoGame)
add_test(NAME potato_test_render COMMAND potato_test_render)

# MingGoRTS-only presentation tests stay outside the PotatoEngine mirror.
add_executable(potato_test_map_projection_spatial
    Examples/MingGoRTS/potato_test_map_projection_spatial.cpp)
target_include_directories(potato_test_map_projection_spatial PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_map_projection_spatial PotatoGame)
add_test(NAME potato_test_map_projection_spatial COMMAND potato_test_map_projection_spatial)

add_executable(potato_test_sprite_animation
    Examples/MingGoRTS/potato_test_sprite_animation.cpp)
target_include_directories(potato_test_sprite_animation PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_sprite_animation PotatoGame)
add_test(NAME potato_test_sprite_animation COMMAND potato_test_sprite_animation)

add_executable(potato_test_framebuffer_clipping
    Examples/MingGoRTS/potato_test_framebuffer_clipping.cpp)
target_include_directories(potato_test_framebuffer_clipping PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_framebuffer_clipping PotatoGame)
add_test(NAME potato_test_framebuffer_clipping COMMAND potato_test_framebuffer_clipping)

# Story 12.9 — overworld view / order menu / creation form
add_executable(potato_test_overworld Examples/MingGoRTS/potato_test_overworld.cpp)
target_include_directories(potato_test_overworld PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_overworld PotatoGame)
add_test(NAME potato_test_overworld COMMAND potato_test_overworld)

# Epic 7 — 不戰 (subversion): defection verb + Subversion verdict +
# negotiated seals + deterrence eval
add_executable(potato_test_subversion Examples/potato_test_subversion.cpp)
target_include_directories(potato_test_subversion PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_subversion PotatoCampaign)
add_test(NAME potato_test_subversion COMMAND potato_test_subversion)
# Story 13.2 — screen-flow state machine (S0 title .. S4 ending)
add_executable(potato_test_screenflow Examples/potato_test_screenflow.cpp)
target_include_directories(potato_test_screenflow PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_screenflow PotatoGame)
add_test(NAME potato_test_screenflow COMMAND potato_test_screenflow)

# Story 13.3 — S0 title screen (menu layout + save-slot scan)
add_executable(potato_test_title Examples/potato_test_title.cpp)
target_include_directories(potato_test_title PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_title PotatoGame)
add_test(NAME potato_test_title COMMAND potato_test_title)

# Story 13.6 — battle session (marshal → deploy → 20 Hz pump → close)
add_executable(potato_test_battlesession
    Examples/potato_test_battlesession.cpp)
target_include_directories(potato_test_battlesession PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_test_battlesession PotatoGame)
add_test(NAME potato_test_battlesession COMMAND potato_test_battlesession)
endif() # POTATO_TESTS

if(POTATO_BUILD_GUI)
# Windowed shell — the only target that may see glfw/glad/imgui.
add_executable(potato_game
    Game/Shell/GlShell.cpp
    Game/Shell/main.cpp)
target_include_directories(potato_game PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(potato_game
    PotatoGame glfw glad imgui ${OPENGL_LIBRARIES})
endif()

if(POTATO_TESTS)
# CLI end-to-end: emit a sealed record, then verify it (exit-code check).
add_test(NAME potato_record_emit
    COMMAND potato_test_record --emit ${CMAKE_BINARY_DIR}/verify_sample.json)
add_test(NAME potato_verify_cli
    COMMAND potato_verify ${CMAKE_BINARY_DIR}/verify_sample.json)
set_tests_properties(potato_verify_cli PROPERTIES DEPENDS potato_record_emit)

if(WIN32)
    # Gameplay 層依賴方向守衛：禁 Rendering/GUI/Platform/GL 標頭
    add_test(NAME gameplay_dep_guard
        # Scope the execution-policy override to this child process. The
        # checked-in guard is a repository script; process scope avoids
        # requiring a machine/user policy change on developer workstations.
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/scripts/CheckGameplayDeps.ps1
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
endif() # POTATO_TESTS

# File-backed tests use the OS temp directory. Point every CTest process at
# its own build-local temp root so tests work in restricted workspaces and
# can run in parallel without colliding on fixed fixture names.
if(WIN32 AND POTATO_TESTS)
    get_property(POTATO_CTEST_NAMES DIRECTORY PROPERTY TESTS)
    foreach(test_name IN LISTS POTATO_CTEST_NAMES)
        set(test_tmp_dir "${CMAKE_BINARY_DIR}/test-tmp/${test_name}")
        file(MAKE_DIRECTORY "${test_tmp_dir}")
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "TEMP=${test_tmp_dir};TMP=${test_tmp_dir}")
    endforeach()
endif()

# 安裝 (暫時禁用)
# install(TARGETS PotatoEngine SimpleExample PlatformTest
#     RUNTIME DESTINATION bin
#     LIBRARY DESTINATION lib
#     ARCHIVE DESTINATION lib
# )

# 安裝頭文件
install(DIRECTORY Core/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Rendering/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Physics/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Audio/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Input/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Resources/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ECS/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY GameObject/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Events/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY FileSystem/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Logging/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY MathUtils/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Memory/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Platform/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Scene/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Serialization/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY Time/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY AI/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")
install(DIRECTORY GUI/ DESTINATION include/PotatoEngine FILES_MATCHING PATTERN "*.h")

# CPack 打包配置
set(CPACK_PACKAGE_NAME "PotatoEngine")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "完全獨立的遊戲引擎")
set(CPACK_PACKAGE_VENDOR "Potato Engine Team")
set(CPACK_GENERATOR "ZIP;TGZ")

include(CPack)
