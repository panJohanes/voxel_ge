#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <cmath>

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#define IMGUI_ENABLE_DOCKING
#include <imgui.h>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_bgfx.h"

namespace fs = std::filesystem;
using json = nlohmann::json;


struct Pos { float x = 0; float y = 0; float z = 0; };
struct Rot { float x = 0; float y = 0; float z = 0; };
struct Scale { float x = 1; float y = 1; float z = 1; };
struct Transform { Pos position; Rot rotation; Scale scale; };

float cam_yaw = 0.785f;
float cam_pitch = 0.615f;
float cam_distance = 35.0f;
Pos camera_pivot = {0.0f, 0.0f, 0.0f};

struct ProjectInfo {
    std::string name;
    std::string path;
    std::string thumbnail = "";
    std::string last_opened = "";
    int64_t created = 0;
};

inline void to_json(json& j, const ProjectInfo& p) {
    j = json{
        {"name", p.name},
        {"path", p.path},
        {"thumbnail", p.thumbnail},
        {"last_opened", p.last_opened},
        {"created", p.created}
    };
}

inline void from_json(const json& j, ProjectInfo& p) {
    j.at("name").get_to(p.name);
    j.at("path").get_to(p.path);
    if (j.contains("thumbnail")) j.at("thumbnail").get_to(p.thumbnail);
    if (j.contains("last_opened")) j.at("last_opened").get_to(p.last_opened);
    if (j.contains("created")) j.at("created").get_to(p.created);
}

class ProjectManager {
private:
    std::string registry_file = "project_registry.json";

public:
    ProjectManager() {
        if (!fs::exists(registry_file)) {
            save(std::vector<ProjectInfo>{});
        }
    }

    std::vector<ProjectInfo> load() {
        try {
            std::ifstream file(registry_file);
            if (!file.is_open()) return {};
            json j;
            file >> j;
            return j.get<std::vector<ProjectInfo>>();
        } catch (...) {
            return {};
        }
    }

    void save(const std::vector<ProjectInfo>& projects) {
        std::ofstream file(registry_file);
        if (file.is_open()) {
            json j = projects;
            file << j.dump(4);
        }
    }

    void add_project(const std::string& path_str) {
        auto projects = load();
        std::string abs_path = fs::absolute(path_str).string();

        for (const auto& proj : projects) {
            if (proj.path == abs_path) return;
        }

        ProjectInfo project;
        project.name = fs::path(abs_path).filename().string();
        project.path = abs_path;
        project.created = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        projects.push_back(project);
        save(projects);
    }

    void remove_project(const std::string& path_str) {
        std::string abs_path = fs::absolute(path_str).string();
        auto projects = load();
        projects.erase(
            std::remove_if(projects.begin(), projects.end(),
                [&abs_path](const ProjectInfo& p) { return p.path == abs_path; }),
            projects.end()
        );
        save(projects);
    }

    std::vector<ProjectInfo> get_projects() {
        auto projects = load();
        std::vector<ProjectInfo> valid;
        for (const auto& proj : projects) {
            if (fs::exists(proj.path)) {
                valid.push_back(proj);
            }
        }
        save(valid);
        return valid;
    }
};

class VoxelFileSystem {
private:
    std::string project_path;

public:
    VoxelFileSystem(std::string path) : project_path(std::move(path)) {}

    std::string create_project(const std::string& name) {
        fs::path path = fs::path(project_path) / name;
        std::vector<std::string> folders = {
            "assets", "scenes", "scripts",
            "assets/models", "assets/textures", "assets/materials", "assets/audio"
        };

        fs::create_directories(path);
        for (const auto& folder : folders) {
            fs::create_directories(path / folder);
        }

        json project = {
            {"name", name},
            {"engine", "VOXEL_GE"},
            {"version", "0.1"}
        };

        std::ofstream file(path / "project.vge");
        if (file.is_open()) {
            file << project.dump(4);
        }
        json default_scene = {
            {"name", name},
            {"suid", 0},
            {"guid", 0},
            {"is_inheret", false},
            {"inhereting_scene", ""},
            {"transform", {
                {"position", {0, 0, 0}},
                {"rotation", {0, 0, 0}},
                {"scale", {1, 1, 1}}
            }},
            {"children", json::object()}
        };
        std::ofstream scene_file(path / "scenes" / "main.json");
        if (scene_file.is_open()) {
            scene_file << default_scene.dump(4);
        }

        return path.string();
    }
};

class Node {
private:
    int cbsuid_max = 0;

public:
    std::string name = "new_node";
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr;
    int suid = 0;
    int guid = 0;
    bool is_inheret = false;
    std::string inhereting_scene = "";
    Transform transform;

    json to_dict() {
        json to_save;
        to_save["name"] = name;
        to_save["suid"] = suid;
        to_save["guid"] = guid;
        to_save["is_inheret"] = is_inheret;
        to_save["inhereting_scene"] = inhereting_scene;

        to_save["transform"]["position"] = std::array<float, 3>{transform.position.x, transform.position.y, transform.position.z};
        to_save["transform"]["rotation"] = std::array<float, 3>{transform.rotation.x, transform.rotation.y, transform.rotation.z};
        to_save["transform"]["scale"] = std::array<float, 3>{transform.scale.x, transform.scale.y, transform.scale.z};
        
        json children_save;
        for (const auto& child : children) {
            if (child) children_save[std::to_string(child->suid)] = child->to_dict();
        }
        to_save["children"] = children_save;
        return to_save;
    }

    void from_dict(const json& j) {
        name = j.value("name", "new_node");
        suid = j.value("suid", 0);
        guid = j.value("guid", 0);
        is_inheret = j.value("is_inheret", false);
        inhereting_scene = j.value("inhereting_scene", "");

        if (j.contains("transform")) {
            if (j["transform"].contains("position") && j["transform"]["position"].is_array() && j["transform"]["position"].size() >= 3) {
                transform.position.x = j["transform"]["position"][0];
                transform.position.y = j["transform"]["position"][1];
                transform.position.z = j["transform"]["position"][2];
            }
            if (j["transform"].contains("rotation") && j["transform"]["rotation"].is_array() && j["transform"]["rotation"].size() >= 3) {
                transform.rotation.x = j["transform"]["rotation"][0];
                transform.rotation.y = j["transform"]["rotation"][1];
                transform.rotation.z = j["transform"]["rotation"][2];
            }
            if (j["transform"].contains("scale") && j["transform"]["scale"].is_array() && j["transform"]["scale"].size() >= 3) {
                transform.scale.x = j["transform"]["scale"][0];
                transform.scale.y = j["transform"]["scale"][1];
                transform.scale.z = j["transform"]["scale"][2];
            }
        }

        children.clear();
        if (j.contains("children") && j["children"].is_object()) {
            for (auto& [key, val] : j["children"].items()) {
                auto child = std::make_unique<Node>();
                child->from_dict(val);
                child->parent = this;
                children.push_back(std::move(child));
            }
        }
    }

    void save_tree(const std::string& filename) {
        json save = to_dict();
        std::ofstream file(filename);
        if (file.is_open()) file << save.dump(4);
    }

    static std::unique_ptr<Node> load_tree(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) return nullptr;
        try {
            json j;
            file >> j;
            auto root = std::make_unique<Node>();
            root->from_dict(j);
            return root;
        } catch (...) {
            return nullptr;
        }
    }

    void add_child(std::unique_ptr<Node> node) {
        if (!node) return;
        node->parent = this;
        node->suid = ++cbsuid_max;
        children.push_back(std::move(node));
    }
};

struct Vertex { float x = 0, y = 0, z = 0, u = 0, v = 0; };
struct Face { Vertex verts[4]; };
class Mesh{
public:
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    void loadMesh(std::string filepath){
        std::cout << "made";
    }
};
//meshinstance
class MeshInstance: public Node{
public:
    std::string active_project_file="";
    std::string name="Mesh";
    std::string mesh_path=active_project_file+"/assets/models/example.obj";
    Mesh mesh;
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout;
    //functions
    void set_project_file(std::string path){
        active_project_file=path;
    }
    void make_mesh(){
        mesh.loadMesh(active_project_file+"/assets/models/example.obj");
    }
    void make_layout(){
        if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
        if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
        if (mesh.vertices.empty() || mesh.indices.empty()) return;
        vbh = bgfx::createVertexBuffer(bgfx::copy(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex)), layout);
        ibh = bgfx::createIndexBuffer(bgfx::copy(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t)), BGFX_BUFFER_INDEX32);
    }
    json to_dict(){
        json to_save=Node::to_dict();
        to_save["mesh_path"]=mesh_path;
        return to_save;
    }
    void from_dict(const json& j){
        Node::from_dict(j);
        mesh_path=j.value("mesh_path","");
    }
    void save_tree(const std::string& filename) {
        json save = to_dict();
        std::ofstream file(filename);
        if (file.is_open()) file << save.dump(4);
    }

    static std::unique_ptr<Node> load_tree(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) return nullptr;
        try {
            json j;
            file >> j;
            auto root = std::make_unique<Node>();
            root->from_dict(j);
            return root;
        } catch (...) {
            return nullptr;
        }
    }
};

bgfx::ShaderHandle loadShader(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) return BGFX_INVALID_HANDLE;
    fseek(file, 0, SEEK_END);
    uint32_t size = static_cast<uint32_t>(ftell(file));
    fseek(file, 0, SEEK_SET);
    const bgfx::Memory* mem = bgfx::alloc(size);
    fread(mem->data, 1, size, file);
    fclose(file);
    return bgfx::createShader(mem);
}

int floorDiv(int a, int b) { return (a >= 0) ? (a / b) : ((a - b + 1) / b); }

int chunk_size = 16;
int chunk_height = 100;

struct ChunkPos {
    int x; int z;
    bool operator==(const ChunkPos& other) const { return x == other.x && z == other.z; }
};

int index(int x, int y, int z) { return x + z * chunk_size + y * chunk_size * chunk_size; }
int worldToLocal(int global) { int local = global % chunk_size; return (local < 0) ? (local + chunk_size) : local; }

namespace std {
template<> struct hash<ChunkPos> {
    size_t operator()(const ChunkPos& p) const { return hash<int>()(p.x) ^ (hash<int>()(p.z) << 1); }
};
}


const Face faces[6] = {
    {{ {0,1,0,0,0}, {0,1,1,1,0}, {1,1,0,0,1}, {1,1,1,1,1} }},
    {{ {0,0,1,0,0}, {0,0,0,1,0}, {1,0,1,0,1}, {1,0,0,1,1} }},
    {{ {1,0,1,0,0}, {1,0,0,1,0}, {1,1,1,0,1}, {1,1,0,1,1} }},
    {{ {0,0,0,0,0}, {0,0,1,1,0}, {0,1,0,0,1}, {0,1,1,1,1} }},
    {{ {0,0,1,0,0}, {1,0,1,1,0}, {0,1,1,0,1}, {1,1,1,1,1} }},
    {{ {1,0,0,0,0}, {0,0,0,1,0}, {1,1,0,0,1}, {0,1,0,1,1} }}
};

class MeshCustom {
public:
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    void addFace(int face, int x, int y, int z) {
        uint32_t start = vertices.size();
        for (int i = 0; i < 4; i++) {
            Vertex v = faces[face].verts[i];
            v.x += x; v.y += y; v.z += z;
            vertices.push_back(v);
        }
        indices.push_back(start + 0); indices.push_back(start + 2); indices.push_back(start + 1);
        indices.push_back(start + 1); indices.push_back(start + 2); indices.push_back(start + 3);
    }
};

struct Block {
    uint8_t type = 0;
    bool isSolid() const { return type != 0; }
};

ChunkPos worldToChunk(int x, int z) { return { floorDiv(x, chunk_size), floorDiv(z, chunk_size) }; }

bool isAir(int globalX, int globalY, int globalZ);

class Chunk {
    ChunkPos position{0,0};
public:
    std::vector<Block> blocks;
    MeshCustom mesh;
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout layout;

    Chunk() { blocks.resize(chunk_size * chunk_height * chunk_size); }
    Chunk(ChunkPos pos) : position(pos) { blocks.resize(chunk_size * chunk_height * chunk_size); }

    Block& getBlock(int x, int y, int z) { return blocks[index(x, y, z)]; }

    void init() {
        layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
                      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
                      .end();
    }
    
    void make_mesh() {
        mesh.vertices.clear(); mesh.indices.clear();
        for (int x = 0; x < chunk_size; x++) {
            for (int y = 0; y < chunk_height; y++) {
                for (int z = 0; z < chunk_size; z++) {
                    Block& block = blocks[index(x, y, z)];
                    if (!block.isSolid()) continue;
                    int gx = position.x * chunk_size + x;
                    int gz = position.z * chunk_size + z;
                    if (isAir(gx + 1, y, gz)) mesh.addFace(2, x, y, z);
                    if (isAir(gx - 1, y, gz)) mesh.addFace(3, x, y, z);
                    if (isAir(gx, y, gz + 1)) mesh.addFace(4, x, y, z);
                    if (isAir(gx, y, gz - 1)) mesh.addFace(5, x, y, z);
                    if (isAir(gx, y - 1, gz)) mesh.addFace(1, x, y, z);
                    if (isAir(gx, y + 1, gz)) mesh.addFace(0, x, y, z);
                }
            }
        }
    }

    void make_layout() {
        if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
        if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
        if (mesh.vertices.empty() || mesh.indices.empty()) return;
        vbh = bgfx::createVertexBuffer(bgfx::copy(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex)), layout);
        ibh = bgfx::createIndexBuffer(bgfx::copy(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t)), BGFX_BUFFER_INDEX32);
    }
};

std::unordered_map<ChunkPos, Chunk> loaded_chunks;

bool isAir(int globalX, int globalY, int globalZ) {
    if (globalY < 0 || globalY >= chunk_height) return true;
    ChunkPos cp = worldToChunk(globalX, globalZ);
    auto it = loaded_chunks.find(cp);
    if (it == loaded_chunks.end()) return true;
    return !it->second.getBlock(worldToLocal(globalX), globalY, worldToLocal(globalZ)).isSolid();
}

void generateChunks() {
    loaded_chunks.clear();
    Chunk chunk({0, 0});
    for (int x = 0; x < chunk_size; x++) {
        for (int z = 0; z < chunk_size; z++) {
            for (int y = 0; y < 5; y++) {
                chunk.getBlock(x, y, z).type = 1;
            }
        }
    }
    loaded_chunks[{0, 0}] = std::move(chunk);
    loaded_chunks[{0, 0}].make_mesh();
}


int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return -1;

    int width = 1280, height = 720;
    SDL_Window* window = SDL_CreateWindow("VOXEL_GE Launcher & Editor", width, height, SDL_WINDOW_RESIZABLE);
    if (!window) return -1;

    bgfx::PlatformData pd{};
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    pd.nwh = (void*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    bgfx::setPlatformData(pd);

    bgfx::Init init;
    init.type = bgfx::RendererType::Count;
    init.resolution.height = height;
    init.resolution.width = width;
    init.resolution.reset = BGFX_RESET_VSYNC;
    init.platformData.nwh = pd.nwh;
    bgfx::init(init);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    #ifdef IMGUI_HAS_DOCK
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    #endif
    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForOther(window);
    ImGui_Implbgfx_Init(255);

    ProjectManager project_manager;
    bool in_launcher = true;
    std::string active_project_path = "";
    std::string current_browser_path = "";
    std::string active_scene_file = "";

    bool show_create_project_modal = false;
    bool show_new_scene_modal = false;
    char project_name_buf[128] = "VOXEL_GE_new_project";
    char project_location_buf[256] = "projects";
    char new_scene_name_buf[128] = "new_scene.json";
    std::string status_message = "Ready";

    bgfx::ShaderHandle vsh = loadShader("shaders/vs_voxel.bin");
    bgfx::ShaderHandle fsh = loadShader("shaders/fs_voxel.bin");
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(vsh) && bgfx::isValid(fsh)) {
        program = bgfx::createProgram(vsh, fsh, true);
    }

    std::unique_ptr<Node> root_node = std::make_unique<Node>();
    Node* selected_node = root_node.get();

    uint16_t rt_width = 800, rt_height = 600;
    bgfx::TextureHandle render_texture = bgfx::createTexture2D(rt_width, rt_height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx::FrameBufferHandle framebuffer = bgfx::createFrameBuffer(1, &render_texture, true);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                width = event.window.data1;
                height = event.window.data2;
                bgfx::reset(width, height, BGFX_RESET_VSYNC);
                bgfx::setViewRect(0, 0, 0, width, height);
            }
            if (event.type == SDL_EVENT_DROP_FILE) {
                if (!in_launcher && !active_project_path.empty()) {
                    std::string dropped_file = event.drop.data;
                    fs::path source_path(dropped_file);
                    std::string ext = source_path.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    fs::path dest_folder;
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
                        dest_folder = fs::path(active_project_path) / "assets" / "textures";
                    } else if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".dae") {
                        dest_folder = fs::path(active_project_path) / "assets" / "models";
                    } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") {
                        dest_folder = fs::path(active_project_path) / "assets" / "audio";
                    } else if (ext == ".mat") {
                        dest_folder = fs::path(active_project_path) / "assets" / "materials";
                    } else if (ext == ".json") {
                        dest_folder = fs::path(active_project_path) / "scenes";
                    } else {
                        dest_folder = current_browser_path.empty() ? (fs::path(active_project_path) / "assets") : current_browser_path;
                    }

                    fs::path dest_path = dest_folder / source_path.filename();
                    try {
                        fs::create_directories(dest_folder);
                        fs::copy_file(source_path, dest_path, fs::copy_options::overwrite_existing);
                        status_message = "Imported to " + dest_folder.filename().string() + ": " + source_path.filename().string();
                    } catch (const std::exception& e) {
                        status_message = std::string("Import failed: ") + e.what();
                    }
                }
            }
        }

        ImGui_Implbgfx_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (in_launcher) {
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2((float)width, (float)height), ImGuiCond_Always);
            ImGui::Begin("VOXEL_GE Launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

            ImGui::TextColored(ImVec4(0.38f, 0.69f, 1.0f, 1.0f), "VOXEL_GE");
            ImGui::SameLine();
            ImGui::Text("Voxel Game Engine Launcher");
            ImGui::Separator();

            auto projects = project_manager.get_projects();
            ImGui::BeginChild("ProjectsScroll", ImVec2(0, -60), true);
            if (projects.empty()) {
                ImGui::Text("No VOXEL_GE projects found. Create one below!");
            } else {
                for (const auto& proj : projects) {
                    ImGui::BeginGroup();
                    ImGui::Button(proj.name.c_str(), ImVec2(200, 40));
                    ImGui::TextWrapped("Path: %s", proj.path.c_str());
                    if (ImGui::Button(("Open##" + proj.path).c_str())) {
                        active_project_path = proj.path;
                        current_browser_path = proj.path;
                        
                        active_scene_file = (fs::path(active_project_path) / "scenes" / "main.json").string();
                        if (fs::exists(active_scene_file)) {
                            root_node = Node::load_tree(active_scene_file);
                            if (!root_node) root_node = std::make_unique<Node>();
                        } else {
                            root_node = std::make_unique<Node>();
                            root_node->name = proj.name;
                        }
                        selected_node = root_node.get();

                        generateChunks();
                        for (auto& [pos, chunk] : loaded_chunks) {
                            chunk.init();
                            chunk.make_layout();
                        }
                        in_launcher = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(("Remove##" + proj.path).c_str())) {
                        project_manager.remove_project(proj.path);
                    }
                    ImGui::EndGroup();
                    ImGui::Separator();
                }
            }
            ImGui::EndChild();

            if (ImGui::Button("+ CREATE PROJECT")) {
                show_create_project_modal = true;
                ImGui::OpenPopup("Create VOXEL_GE Project");
            }

            if (ImGui::BeginPopupModal("Create VOXEL_GE Project", &show_create_project_modal, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Create VOXEL_GE Project");
                ImGui::Separator();

                ImGui::Text("Project Name");
                ImGui::InputText("##ProjectName", project_name_buf, sizeof(project_name_buf));

                ImGui::Text("Location");
                ImGui::InputText("##ProjectLocation", project_location_buf, sizeof(project_location_buf));

                ImGui::Separator();
                if (ImGui::Button("CREATE PROJECT", ImVec2(140, 0))) {
                    VoxelFileSystem vfs(project_location_buf);
                    std::string created_path = vfs.create_project(project_name_buf);
                    project_manager.add_project(created_path);
                    
                    show_create_project_modal = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                    show_create_project_modal = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::End();

        } else {
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("New Scene")) {
                        show_new_scene_modal = true;
                    }
                    if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                        if (!active_scene_file.empty()) {
                            root_node->save_tree(active_scene_file);
                            status_message = "Scene saved: " + fs::path(active_scene_file).filename().string();
                        }
                    }
                    if (ImGui::BeginMenu("Open Scene")) {
                        fs::path scenes_dir = fs::path(active_project_path) / "scenes";
                        try {
                            for (const auto& entry : fs::directory_iterator(scenes_dir)) {
                                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                                    std::string scene_path = entry.path().string();
                                    std::string scene_name = entry.path().filename().string();
                                    if (ImGui::MenuItem(scene_name.c_str())) {
                                        active_scene_file = scene_path;
                                        auto loaded = Node::load_tree(active_scene_file);
                                        if (loaded) {
                                            root_node = std::move(loaded);
                                            selected_node = root_node.get();
                                            status_message = "Loaded scene: " + scene_name;
                                        } else {
                                            status_message = "Failed to load scene: " + scene_name;
                                        }
                                    }
                                }
                            }
                        } catch (...) {}
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Return to Launcher")) {
                        in_launcher = true;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }
            if (show_new_scene_modal) {
                ImGui::OpenPopup("Create New Scene");
                show_new_scene_modal = false;
            }
            if (ImGui::BeginPopupModal("Create New Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Enter Scene Filename (.json):");
                ImGui::InputText("##NewSceneName", new_scene_name_buf, sizeof(new_scene_name_buf));
                ImGui::Separator();
                if (ImGui::Button("Create", ImVec2(120, 0))) {

                    std::string filename = new_scene_name_buf;
                    if (filename.find(".json") == std::string::npos) filename += ".json";
                    fs::path new_scene_path = fs::path(active_project_path) / "scenes" / filename;
                    
                    root_node = std::make_unique<Node>();
                    root_node->name = fs::path(filename).stem().string();
                    root_node->save_tree(new_scene_path.string());
                    
                    active_scene_file = new_scene_path.string();
                    selected_node = root_node.get();
                    status_message = "Created new scene: " + filename;

                    show_new_scene_modal = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                    show_new_scene_modal = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Begin("Scene Hierarchy");
            ImGui::Text("Active Scene: %s", fs::path(active_scene_file).filename().string().c_str());
            ImGui::Separator();
            std::function<void(Node*)> draw_node_tree = [&](Node* node) {
                if (!node) return;
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (node == selected_node) flags |= ImGuiTreeNodeFlags_Selected;
                if (node->children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
                
                bool opened = ImGui::TreeNodeEx((void*)node, flags, "%s", node->name.c_str());
                if (ImGui::IsItemClicked()) selected_node = node;
                if (opened) {
                    for (auto& child : node->children) draw_node_tree(child.get());
                    ImGui::TreePop();
                }
            };
            draw_node_tree(root_node.get());
            ImGui::End();

            ImGui::Begin("File System");
            if (!current_browser_path.empty()) {
                if (current_browser_path != active_project_path) {
                    if (ImGui::Button(".. (Up Directory)")) {
                        current_browser_path = fs::path(current_browser_path).parent_path().string();
                    }
                    ImGui::Separator();
                }
                ImGui::TextWrapped("Current: %s", current_browser_path.c_str());
                ImGui::Separator();

                ImGui::BeginChild("FileList", ImVec2(0, 0), true);
                try {
                    for (const auto& entry : fs::directory_iterator(current_browser_path)) {
                        auto filename = entry.path().filename().string();
                        if (entry.is_directory()) {
                            if (ImGui::Selectable(("[DIR] " + filename).c_str(), false)) {
                                current_browser_path = entry.path().string();
                            }
                        } else {
                            if (ImGui::Selectable(filename.c_str(), false)) {
                                if (entry.path().extension() == ".json" && entry.path().parent_path().filename() == "scenes") {
                                    active_scene_file = entry.path().string();
                                    auto loaded = Node::load_tree(active_scene_file);
                                    if (loaded) {
                                        root_node = std::move(loaded);
                                        selected_node = root_node.get();
                                        status_message = "Loaded scene: " + filename;
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {
                    ImGui::Text("Could not read directory contents.");
                }
                ImGui::EndChild();
            }
            ImGui::End();

            ImGui::Begin("Inspector");
            if (selected_node) {
                char name_buf[256];
                memset(name_buf, 0, sizeof(name_buf));
                strncpy(name_buf, selected_node->name.c_str(), sizeof(name_buf) - 1);
                if (ImGui::InputText("Node Name", name_buf, sizeof(name_buf))) selected_node->name = name_buf;

                ImGui::Separator();
                ImGui::DragFloat3("Position", &selected_node->transform.position.x, 0.1f);
                ImGui::DragFloat3("Rotation", &selected_node->transform.rotation.x, 0.5f);
                ImGui::DragFloat3("Scale", &selected_node->transform.scale.x, 0.01f, 0.01f, 100.0f);

                ImGui::Separator();
                if (ImGui::Button("Add Child Node")) {
                    ImGui::OpenPopup("AddNodePopup");
                }

                if (ImGui::BeginPopup("AddNodePopup")) {
                    ImGui::Text("Select Node Type");
                    ImGui::Separator();
                    
                    const char* node_types[] = { "Node", "MeshInstance", "Camera3D", "DirectionalLight", "CollisionShape" };
                    for (int i = 0; i < IM_ARRAYSIZE(node_types); i++) {
                        if (ImGui::Selectable(node_types[i])) {
                            if (i==0){
                                auto new_child = std::make_unique<Node>();
                                new_child->name = std::string(node_types[i]) + "_" + std::to_string(selected_node->children.size() + 1);
                                selected_node->add_child(std::move(new_child));
                            }
                            else if (i==1){
                                auto new_child = std::make_unique<MeshInstance>();
                                new_child->name = std::string(node_types[i]) + "_" + std::to_string(selected_node->children.size() + 1);
                                new_child->set_project_file(active_project_path);
                                selected_node->add_child(std::move(new_child));
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::End();

            ImGui::Begin("Engine Stats");
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Frame Time: %.3f ms", 1000.0f / ImGui::GetIO().Framerate);
            ImGui::Separator();
            ImGui::Text("Loaded Chunks: %zu", loaded_chunks.size());
            size_t total_verts = 0;
            size_t total_indices = 0;
            for (auto& [pos, chunk] : loaded_chunks) {
                total_verts += chunk.mesh.vertices.size();
                total_indices += chunk.mesh.indices.size();
            }
            ImGui::Text("Total Vertices: %zu", total_verts);
            ImGui::Text("Total Indices: %zu", total_indices);
            ImGui::Separator();
            ImGui::Text("Status: %s", status_message.c_str());
            ImGui::End();

            ImGui::Begin("Viewport");
            ImVec2 panel_size = ImGui::GetContentRegionAvail();
            if (panel_size.x > 0 && panel_size.y > 0) {
                uint16_t nw = static_cast<uint16_t>(panel_size.x);
                uint16_t nh = static_cast<uint16_t>(panel_size.y);
                if (nw != rt_width || nh != rt_height) {
                    rt_width = nw; rt_height = nh;
                    bgfx::destroy(framebuffer);
                    bgfx::destroy(render_texture);
                    render_texture = bgfx::createTexture2D(rt_width, rt_height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                    framebuffer = bgfx::createFrameBuffer(1, &render_texture, true);
                }
            }


            ImGui::Image((ImTextureID)(uintptr_t)render_texture.idx, panel_size);


            if (ImGui::IsItemHovered()) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;

                
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && selected_node) {
                    camera_pivot = {0.0f, 0.0f, 0.0f};
                    for (const Node* curr = selected_node; curr != nullptr; curr = curr->parent) {
                        camera_pivot.x += curr->transform.position.x;
                        camera_pivot.y += curr->transform.position.y;
                        camera_pivot.z += curr->transform.position.z;
                    }
                }

                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    float rot_sensitivity = 0.005f;
                    cam_yaw += delta.x * rot_sensitivity;
                    cam_pitch += delta.y * rot_sensitivity;

                    const float max_pitch = 1.55f;
                    if (cam_pitch > max_pitch) cam_pitch = max_pitch;
                    if (cam_pitch < -max_pitch) cam_pitch = -max_pitch;
                }

                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && selected_node) {
                    float drag_sensitivity = 0.05f;
                    selected_node->transform.position.x += delta.x * drag_sensitivity;
                    selected_node->transform.position.y -= delta.y * drag_sensitivity;
                }
            }

            float eye_x = camera_pivot.x + cam_distance * cosf(cam_pitch) * sinf(cam_yaw);
            float eye_y = camera_pivot.y + cam_distance * sinf(cam_pitch);
            float eye_z = camera_pivot.z + cam_distance * cosf(cam_pitch) * cosf(cam_yaw);

            float view[16], proj[16], viewProj[16];
            bx::mtxLookAt(view, bx::Vec3{eye_x, eye_y, eye_z}, bx::Vec3{camera_pivot.x, camera_pivot.y, camera_pivot.z});
            bx::mtxProj(proj, 60.0f, float(rt_width) / float(rt_height), 0.1f, 1000.0f, bgfx::getCaps()->homogeneousDepth);
            bx::mtxMul(viewProj, view, proj);

            if (selected_node) {
                Pos world_pos = {0.0f, 0.0f, 0.0f};
                for (const Node* curr = selected_node; curr != nullptr; curr = curr->parent) {
                    world_pos.x += curr->transform.position.x;
                    world_pos.y += curr->transform.position.y;
                    world_pos.z += curr->transform.position.z;
                }

                float clipX = world_pos.x * viewProj[0] + world_pos.y * viewProj[4] + world_pos.z * viewProj[8]  + viewProj[12];
                float clipY = world_pos.x * viewProj[1] + world_pos.y * viewProj[5] + world_pos.z * viewProj[9]  + viewProj[13];
                float clipZ = world_pos.x * viewProj[2] + world_pos.y * viewProj[6] + world_pos.z * viewProj[10] + viewProj[14];
                float clipW = world_pos.x * viewProj[3] + world_pos.y * viewProj[7] + world_pos.z * viewProj[11] + viewProj[15];

                if (clipW > 0.1f) {
                    float ndcX = clipX / clipW;
                    float ndcY = clipY / clipW;

                    ImVec2 image_min = ImGui::GetItemRectMin();
                    ImVec2 image_size = ImGui::GetItemRectSize();

                    float screenX = image_min.x + (ndcX + 1.0f) * 0.5f * image_size.x;
                    float screenY = image_min.y + (1.0f - ndcY) * 0.5f * image_size.y;

                    if (screenX >= image_min.x && screenX <= image_min.x + image_size.x &&
                        screenY >= image_min.y && screenY <= image_min.y + image_size.y) 
                    {
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        draw_list->AddCircleFilled(ImVec2(screenX, screenY), 6.0f, IM_COL32(255, 140, 0, 255));
                        draw_list->AddCircle(ImVec2(screenX, screenY), 7.0f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                    }
                }
            }

            ImGui::End();

            bgfx::setViewFrameBuffer(0, framebuffer);
            bgfx::setViewRect(0, 0, 0, rt_width, rt_height);
            bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff);
            bgfx::setViewTransform(0, view, proj);

            if (bgfx::isValid(program)) {
                for (auto& [pos, chunk] : loaded_chunks) {
                    if (!bgfx::isValid(chunk.vbh)) continue;
                    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW);
                    bgfx::setVertexBuffer(0, chunk.vbh);
                    bgfx::setIndexBuffer(chunk.ibh);
                    bgfx::submit(0, program);
                }
            }
        }

        bgfx::touch(0);
        ImGui::Render();
        ImGui_Implbgfx_RenderDrawLists(ImGui::GetDrawData());
        bgfx::frame();
    }

    bgfx::destroy(framebuffer);
    ImGui_Implbgfx_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}