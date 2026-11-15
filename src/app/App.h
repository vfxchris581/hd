#pragma once
#include "core/Lights.h"
#include "core/Node.h"
#include "core/PathTracer.h"
#include "render/Renderer.h"
#include <set>
#include <string>
#include <vector>

struct GLFWwindow;
struct ImFont;

namespace hd {

class App;

// Houdini-style network editor (vertical flow, Tab menu, flags, wires).
class NetworkEditor {
public:
    void draw(App& app);
    void frameAll(App& app);
    void requestFrameAll() { wantFrameAll_ = true; }

private:
    enum class Drag { None, Pan, Move, Wire, Box };
    struct Vec2 { float x = 0, y = 0; };
    Vec2 offset_{60, 60};
    float zoom_ = 1.f;
    Drag drag_ = Drag::None;
    bool moveCheckpointed_ = false;
    int wireSrc_ = -1;            // node id whose output is being dragged
    int wireDetachedDst_ = -1;    // when re-wiring from an input
    int wireDetachedIdx_ = -1;
    Vec2 boxStart_;
    bool wantFrameAll_ = true;
    int frameAllDelay_ = 0;
    // Tab menu
    bool openMenu_ = false;
    Vec2 menuWorld_;
    int menuConnectFrom_ = -1;
    char search_[64] = {0};
    int menuHighlight_ = 0;
    // rename
    int diveId_ = -1;
    bool upRequested_ = false;
    int renameId_ = -1;
    char renameBuf_[64] = {0};
    void drawNodeMenu(App& app);
};

enum class FileDialogMode { None, Open, SaveAs, ImportObj, ExportObj, ChooseFileParm, SaveImage, SaveSequence, MergeScene, InstallAsset };

class App {
public:
    bool init(GLFWwindow* window, const std::string& startupFile);
    void frame();
    void shutdown();
    bool handleCloseRequest(); // returns true if it is OK to close now
    void openPath(const std::string& path); // drag & drop / command line

    // --- state shared with panels
    Graph graph;
    Renderer renderer;
    Camera camera;
    ViewOptions viewOpts;
    bool showPointNumbers = false;
    bool showPrimNumbers = false;
    std::set<int> selection;   // node ids
    int currentId = -1;        // node shown in the parameter pane
    NetworkEditor network;
    ImFont* monoFont = nullptr;

    // The network level being edited: the scene itself, or the inside of a subnet.
    Graph& net();
    Node* current() { return net().findNode(currentId); }
    const std::vector<std::string>& netPath() const { return netPath_; }
    void diveInto(Node* subnetNode);
    void goToLevel(size_t depth); // 0 = top level
    void collapseSelected();
    // reusable assets (saved subnets)
    std::string assetDir() const;
    const std::vector<std::string>& assets() const { return assets_; }
    void refreshAssets();
    void saveAsset(Node* subnetNode);
    Node* createAsset(const std::string& name, float x, float y, int connectFrom = -1);
    void deleteAsset(const std::string& name);
    void installAsset(const std::string& path);
    void drawAssetManager();
    bool showAssetManager = false;
    // shelf, lights and cameras (Shelf.cpp)
    void drawShelf();
    void shelfTool(const std::string& tool);
    void updateSceneCamera(bool navigated, float viewAspect); // look-through / lock handling
    void updateGuides();
    Node* activeCameraNode();
    bool showShelf = true;
    bool lockCamera = false;
    float activeCamAspect = 0; // > 0 while looking through a camera node
    bool camEditActive_ = false;
    void select(Node* n, bool additive = false);
    void clearSelection() { selection.clear(); currentId = -1; }

    // undo
    void checkpoint();
    void undo();
    void redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void markDirty() { dirty_ = true; }

    // actions
    Node* createNode(const std::string& type, float x, float y, int connectFrom = -1);
    void deleteSelected();
    void duplicateSelected();
    void setDisplay(Node* n);
    void newScene(bool withDemo);
    bool loadScene(const std::string& path);
    bool saveScene(const std::string& path);
    void openFileDialog(FileDialogMode mode);
    void status(const std::string& msg, bool error = false);
    void frameGeometry();

    // panels
    void drawViewport();
    void drawParameters();
    void drawSpreadsheet();
    void drawTimeline();
    void drawRenderView();
    void startRender();
    bool rendering() const { return tracer_.running(); }
    void drawStatusLine();
    void drawMenuBar();
    void drawFileDialog();
    void drawHelpWindows();

    // menus, preferences, clipboard (Menus.cpp)
    void drawDisplayOptions();
    void drawRenderSettings();
    void stepSequence();
    void loadPrefs();
    void savePrefs();
    void pushRecent(const std::string& path);
    void copySelected();
    void cutSelected();
    void pasteNodes();
    void mergeScene(const std::string& path);
    void saveIncrement();
    void openExample(const std::string& name);
    bool showSceneView = true, showNetwork = true, showParms = true, showSpreadsheet = true, showRenderView = true, showTimeline = true;
    bool showDisplayOptions = false, showRenderSettings = false;

    bool playing = false;
    bool realtime = true;
    bool showShortcuts = false;
    bool showVexHelp = false;
    bool showAbout = false;
    bool showImGuiDemo = false;
    std::string fileParmTarget; // "nodeName/parmName" for ChooseFileParm
    std::string examplesDir;    // bundled example scenes (next to the executable)

private:
    void buildDefaultLayout(unsigned dockId);
    void handleGlobalShortcuts();
    void updateTitle();
    void cookDisplay();

    struct Snapshot { std::string graph; std::vector<std::string> selected; std::string current; std::vector<std::string> path; };
    Snapshot snapshot();
    void restore(const Snapshot& s);
    std::vector<Snapshot> undo_, redo_;

    GLFWwindow* window_ = nullptr;
    std::string currentFile_;
    bool dirty_ = false;
    bool layoutBuilt_ = false;
    bool forceLayout_ = false;
    int focusSceneView_ = 0; // frames until the Scene View tab is brought to the front
    bool confirmClose_ = false;
    bool allowClose_ = false;
    double lastTime_ = 0;
    double playAccum_ = 0;
    std::string statusMsg_;
    bool statusErr_ = false;
    double statusTime_ = 0;
    std::string lastTitle_;
    float viewportW_ = 0, viewportH_ = 0;
    bool frameOnNextCook_ = true;

    // render view
    PathTracer tracer_;
    unsigned renderTex_ = 0;
    int renderW_ = 0, renderH_ = 0;
    uint64_t renderVersion_ = 0;
    std::vector<uint8_t> renderImg_;
    bool seqActive_ = false, seqWaiting_ = false;
    int seqFrame_ = 0, seqEnd_ = 0, seqCount_ = 0;
    std::string seqBase_;
    void syncCameraToScene();

    std::vector<std::string> recent_;
    std::vector<std::string> netPath_; // names of the subnets we have dived into
    std::vector<std::string> assets_;
    std::string clipboard_;

    // file dialog
    FileDialogMode fileMode_ = FileDialogMode::None;
    bool fileDialogOpenRequest_ = false;
    std::string fileDir_;
    char fileName_[512] = {0};

    friend class NetworkEditor;
};

// Style
void applyHoudiniTheme(float scale);

} // namespace hd
