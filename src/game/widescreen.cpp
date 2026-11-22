// The picture in a window wider than 16:9. The guest still renders 1280x720, which the present
// stretches over the window (gpu::WideStretch). Here the game squeezes what it draws by the same
// amount, so that it comes out with its own proportions: the cameras see a wider field, and the
// interface's movies, images, texts and videos keep their shape in a 16:9 frame of the picture,
// except the ones made to span the screen, which stay stretched over it.

#include "game/widescreen.h"

#include "game/addresses.h"
#include "gpu/gpu.h"
#include "kernel/kernel.h"

#include <atomic>
#include <bit>
#include <string>
#include <string_view>
#include <vector>

DECLARE_ORIGINAL(JADE_CAMERA_ASSIGN);
DECLARE_ORIGINAL(JADE_INTERFACE_PLACE);
DECLARE_ORIGINAL(JADE_INTERFACE_MOVIE_DRAW);
DECLARE_ORIGINAL(JADE_INTERFACE_IMAGE_DRAW);
DECLARE_ORIGINAL(JADE_INTERFACE_TEXT_DRAW);
DECLARE_ORIGINAL(JADE_VIDEO_QUAD);
DECLARE_ORIGINAL(JADE_VERTEX_ALLOCATE);
DECLARE_ORIGINAL(JADE_RECORD_SUBMIT);
DECLARE_ORIGINAL(JADE_RECORD_DRAW);

namespace
{
    namespace jade = game::jade;

    // How a movie of the interface is drawn in a wide picture.
    enum class Shape
    {
        Centered,  // with its own proportions, in a 16:9 frame at the middle, clipped to that frame
        Edges,     // likewise, its left half in a frame at the left edge and its right half in one at the right
        Filled,    // with its own proportions, enlarged to span the picture, its top and bottom cut off
        Stretched, // over the whole picture, as wide as the present makes it
    };

    // A movie by its name in lower case with backslashes; a final '*' stands for any ending.
    struct Rule
    {
        std::string_view name;
        Shape shape;
    };

    // The movies that are not centered. Everything else is: menus, dialogues, portraits, messages,
    // the rest of the HUD.
    constexpr Rule kRules[] = {
        // Covers of the whole screen, which have no shape to keep.
        { "black_fade", Shape::Stretched },
        { "blackscreen", Shape::Stretched },
        { "fadeinout", Shape::Stretched },
        { "fadetoblack", Shape::Stretched },
        { "white_fade", Shape::Stretched },
        { "white_flash", Shape::Stretched },
        { "wipe_*", Shape::Stretched },
        { "speedlines_*", Shape::Stretched },
        { "damage", Shape::Stretched },
        { "scriptedevent\\blackstripes", Shape::Stretched },
        { "rage\\rageblackoverlay", Shape::Stretched },
        { "kosystem\\ko_flash", Shape::Stretched },
        // The start of a fight and its pause: bands and a shade drawn from one edge of the screen to
        // the other, around art that sits against them.
        { "hud_fight_start_*", Shape::Stretched },
        { "menu\\menu_fightpause", Shape::Stretched },
        // Placed by the scripts on a fighter's position on the screen, which is already the squeezed one.
        { "hud_handseals", Shape::Stretched },
        { "chakracharge", Shape::Stretched },
        // The open world's button panel, at the right edge of the screen.
        { "controlpanel", Shape::Edges },
        // The pictures of a jutsu: flashes and drawings over the whole screen, in front of the scene
        // or behind the fighters.
        { "8trigrams64palms_*", Shape::Filled },
        { "armorofsand", Shape::Filled },
        { "chidori_*", Shape::Filled },
        { "death_forseeing", Shape::Filled },
        { "fireball_*", Shape::Filled },
        { "gaara_transition", Shape::Filled },
        { "haku_*", Shape::Filled },
        { "kakashi_transition*", Shape::Filled },
        { "kiba_*", Shape::Filled },
        { "orochimaru_transition", Shape::Filled },
        { "sexy_*", Shape::Filled },
        { "shadowrep_*", Shape::Filled },
        { "sharingan_*", Shape::Filled },
        { "summoning", Shape::Filled },
        { "zabuza_transition", Shape::Filled },
    };

    // The HUD with the portrait and the bars, on a band that runs to the edge of the screen. The open
    // world shows one, which goes to the picture's edges; a fight shows one per fighter, with bands
    // from edge to edge, so those stay stretched.
    constexpr std::string_view kStatusHud = "hud53";
    constexpr uint32_t kFightHuds = 2;

    // A part of the screen, in shares of its width and height: what a placement of the interface holds.
    struct Frame
    {
        float left = 0.0f;
        float top = 0.0f;
        float width = 1.0f;
        float height = 1.0f;
    };

    // What the item being drawn on this thread still has to get.
    struct Item
    {
        bool framed = false;         // the next placement goes inside frame
        Frame frame;
        bool unlessCovering = false; // ...unless that placement spans the screen's width
        bool clipped = false;        // the records submitted are marked with clip
        gpu::DrawClip clip;
    };

    // The draw records of a packet that are clipped: ranges of addresses, in the order they were filled.
    struct Marks
    {
        struct Range
        {
            uint32_t begin = 0;
            uint32_t end = 0;
            gpu::DrawClip clip;
        };
        std::vector<Range> ranges;
    };

    thread_local Item t_item;
    // Inside JADE_VIDEO_QUAD: the vertices it allocated.
    thread_local bool t_videoQuad = false;
    thread_local uint32_t t_videoVertices = 0;

    // One per packet of the renderer. A packet's marks are written by the thread that builds it and
    // read by the render thread once it is handed over, never at the same time.
    Marks g_marks[jade::renderer::kPacketCount];
    // The stretch the packet being built is drawn for: gpu::WideStretch at the last hand-off, so that
    // the camera and every item of a packet agree while the window is being resized.
    std::atomic<float> g_stretch{ 1.0f };

    float LoadFloat(uint32_t address)
    {
        return std::bit_cast<float>(mem::Load32(address));
    }

    void StoreFloat(uint32_t address, float value)
    {
        mem::Store32(address, std::bit_cast<uint32_t>(value));
    }

    // The marks of a packet of the renderer; null for any other address.
    Marks* MarksOf(uint32_t packet)
    {
        uint32_t offset = packet - mem::Load32(jade::kRenderer);
        uint32_t index = offset / jade::renderer::kPacketSize;
        if (index >= jade::renderer::kPacketCount || offset % jade::renderer::kPacketSize != 0)
            return nullptr;
        return &g_marks[index];
    }

    // A movie item's name as the rules write it; empty when it has none in 8-bit characters.
    std::string MovieName(uint32_t item)
    {
        uint32_t name = item + jade::interface_item::kMovieName;
        uint32_t characters = mem::Load32(name);
        if (characters == 0 || (mem::Load32(name + jade::interface_item::kNameFlags) & jade::interface_item::kNameWide) != 0)
            return {};
        std::string text = mem::String(characters);
        for (char& c : text)
            c = c == '/' ? '\\' : (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        return text;
    }

    // How many status HUDs the interface manager shows. Inside its draw, which holds its lock.
    uint32_t StatusHudsShown()
    {
        using namespace jade::interface_item;
        uint32_t manager = mem::Load32(jade::kEffects);
        uint32_t groups = mem::Load32(manager + jade::interface_manager::kGroups);
        uint32_t shown = 0;
        for (uint32_t g = 0, groupCount = mem::Load32(manager + jade::interface_manager::kGroupCount); g < groupCount; g++)
        {
            uint32_t group = mem::Load32(groups + 4 * g);
            if (group == 0 || (mem::Load32(group + kFlags) & kShown) != kShown)
                continue;
            uint32_t items = mem::Load32(group + kGroupItems);
            for (uint32_t i = 0, itemCount = mem::Load32(group + kGroupItemCount); i < itemCount; i++)
            {
                uint32_t item = mem::Load32(items + 4 * i);
                if (item != 0 && mem::Load32(item + kVtable) == kMovieVtable && (mem::Load32(item + kFlags) & kShown) == kShown &&
                    mem::Load32(item + kMovie) != 0 && MovieName(item) == kStatusHud)
                    shown++;
            }
        }
        return shown;
    }

    // The shape of a movie about to be drawn.
    Shape ShapeOf(std::string_view movie)
    {
        if (movie == kStatusHud)
            return StatusHudsShown() >= kFightHuds ? Shape::Stretched : Shape::Edges;
        for (const Rule& rule : kRules)
        {
            bool prefix = rule.name.ends_with('*');
            std::string_view name = prefix ? rule.name.substr(0, rule.name.size() - 1) : rule.name;
            if (prefix ? movie.starts_with(name) : movie == name)
                return rule.shape;
        }
        return Shape::Centered;
    }

    // The 16:9 frame of a picture stretched that much, as far into the margin the picture has beside
    // one as the given share of it: 0 at the left edge, 0.5 at the middle, 1 at the right edge.
    Frame NarrowFrame(float stretch, float margin)
    {
        return { (1.0f - 1.0f / stretch) * margin, 0.0f, 1.0f / stretch, 1.0f };
    }

    // Draws an item inside a frame instead of the whole screen. With a clip, the draws it submits are
    // clipped to it.
    void DrawFramed(PPCContext& ctx, uint8_t* base, PPCFunc* original, const Frame& frame, bool unlessCovering = false,
        const gpu::DrawClip* clip = nullptr)
    {
        t_item = { true, frame, unlessCovering, clip != nullptr, clip != nullptr ? *clip : gpu::DrawClip{} };
        original(ctx, base);
        t_item = {};
    }
}

void game::widescreen::PacketHanded()
{
    uint32_t renderer = mem::Load32(jade::kRenderer);
    if (Marks* marks = MarksOf(renderer + jade::renderer::kPacketSize * mem::Load32(renderer + jade::renderer::kBuiltPacket)))
        marks->ranges.clear();
    g_stretch.store(gpu::WideStretch(), std::memory_order_relaxed);
}

// CAM_AssignCameraToDevice (82584860; r3 the screen format, r4 the camera, r5 and r6 the device's width
// and height): from the screen ratio (height over width, 0.5625 for the 16:9 the game always sets) and
// the field of view, which is vertical, it writes the camera's size on the device and the factors the
// projection (820E8600), the culling and the screen positions of world points are built from. The
// view block calls it for the display's camera before each display. Hooked to narrow what the camera
// draws by the stretch: the horizontal factor, and the ratio the camera keeps for the items that size
// themselves by it, as if the screen ratio were the window's. The game's own table of ratios
// (826F50FC) would do the same, but it also shrinks the camera's size on the device to a band, and the
// interface places itself by that size.
PPC_FUNC(JADE_CAMERA_ASSIGN)
{
    uint32_t camera = ctx.r4.u32;
    ORIGINAL(JADE_CAMERA_ASSIGN)(ctx, base);
    float stretch = g_stretch.load(std::memory_order_relaxed);
    if (stretch == 1.0f || (mem::Load32(camera + jade::camera::kFlags) & jade::camera::kFlagPerspective) == 0)
        return;
    StoreFloat(camera + jade::camera::kFactorX, LoadFloat(camera + jade::camera::kFactorX) / stretch);
    StoreFloat(camera + jade::camera::kScreenRatio, LoadFloat(camera + jade::camera::kScreenRatio) / stretch);
}

// The placement of an interface item (820C3A58; r3 the result, r4 the item's own, r5 the one it is
// in): every draw of the interface starts with it, and a movie's draw calls it a second time for the
// movie inside the item. Hooked to put the first placement of a framed item inside its frame, which
// the placements inside it then follow.
PPC_FUNC(JADE_INTERFACE_PLACE)
{
    uint32_t placement = ctx.r3.u32;
    ORIGINAL(JADE_INTERFACE_PLACE)(ctx, base);
    if (!t_item.framed)
        return;
    t_item.framed = false;
    const Frame& frame = t_item.frame;
    float left = LoadFloat(placement + jade::placement::kLeft);
    float width = LoadFloat(placement + jade::placement::kWidth);
    if (t_item.unlessCovering && left <= 0.0f && left + width >= 1.0f)
        return;
    StoreFloat(placement + jade::placement::kLeft, frame.left + frame.width * left);
    StoreFloat(placement + jade::placement::kWidth, frame.width * width);
    StoreFloat(placement + jade::placement::kTop, frame.top + frame.height * LoadFloat(placement + jade::placement::kTop));
    StoreFloat(placement + jade::placement::kHeight, frame.height * LoadFloat(placement + jade::placement::kHeight));
}

// The draw of a Flash movie item (826A42B8; r3 the item, r4 the placement it is in): it places the
// item and displays its movie (826B03E0), which only draws; the movie advances in the item's update.
// Hooked to draw the movie with the shape its name asks for; a movie that goes to the edges is drawn
// twice, a half each time.
PPC_FUNC(JADE_INTERFACE_MOVIE_DRAW)
{
    float stretch = g_stretch.load(std::memory_order_relaxed);
    Shape shape = stretch == 1.0f ? Shape::Stretched : ShapeOf(MovieName(ctx.r3.u32));
    switch (shape)
    {
    case Shape::Stretched:
        ORIGINAL(JADE_INTERFACE_MOVIE_DRAW)(ctx, base);
        break;
    case Shape::Filled:
        DrawFramed(ctx, base, ORIGINAL(JADE_INTERFACE_MOVIE_DRAW), { 0.0f, (1.0f - stretch) / 2, 1.0f, stretch });
        break;
    case Shape::Centered:
    {
        Frame frame = NarrowFrame(stretch, 0.5f);
        gpu::DrawClip clip{ frame.left, frame.left + frame.width };
        DrawFramed(ctx, base, ORIGINAL(JADE_INTERFACE_MOVIE_DRAW), frame, false, &clip);
        break;
    }
    case Shape::Edges:
    {
        PPCRegister item = ctx.r3, placement = ctx.r4;
        Frame frame = NarrowFrame(stretch, 0.0f);
        gpu::DrawClip clip{ 0.0f, frame.width / 2 };
        DrawFramed(ctx, base, ORIGINAL(JADE_INTERFACE_MOVIE_DRAW), frame, false, &clip);
        ctx.r3 = item;
        ctx.r4 = placement;
        frame = NarrowFrame(stretch, 1.0f);
        clip = { 1.0f - frame.width / 2, 1.0f };
        DrawFramed(ctx, base, ORIGINAL(JADE_INTERFACE_MOVIE_DRAW), frame, false, &clip);
        break;
    }
    }
}

// The draw of an image item (8264F1A0; r3 the item, r4 the placement it is in): one quad, plain or
// with a texture (a save's thumbnail). Hooked to center it, unless it spans the screen (a fade).
PPC_FUNC(JADE_INTERFACE_IMAGE_DRAW)
{
    float stretch = g_stretch.load(std::memory_order_relaxed);
    if (stretch == 1.0f)
        ORIGINAL(JADE_INTERFACE_IMAGE_DRAW)(ctx, base);
    else
        DrawFramed(ctx, base, ORIGINAL(JADE_INTERFACE_IMAGE_DRAW), NarrowFrame(stretch, 0.5f), true);
}

// The draw of a text item (8264F768; r3 the item, r4 the placement it is in). Hooked to center it.
PPC_FUNC(JADE_INTERFACE_TEXT_DRAW)
{
    float stretch = g_stretch.load(std::memory_order_relaxed);
    if (stretch == 1.0f)
        ORIGINAL(JADE_INTERFACE_TEXT_DRAW)(ctx, base);
    else
        DrawFramed(ctx, base, ORIGINAL(JADE_INTERFACE_TEXT_DRAW), NarrowFrame(stretch, 0.5f));
}

// The frame of a Bink video as a quad (820EC008; r3 the video), from the draw of a video item
// (82650580): four vertices in clip space, from the frame's size and position in device pixels,
// written into vertex memory it takes from JADE_VERTEX_ALLOCATE and drawn by the record it submits.
// Hooked to squeeze the quad towards the middle of the screen: the record is drawn only after the
// packet is handed over, so the vertices can still be changed.
PPC_FUNC(JADE_VIDEO_QUAD)
{
    float stretch = g_stretch.load(std::memory_order_relaxed);
    if (stretch == 1.0f)
    {
        ORIGINAL(JADE_VIDEO_QUAD)(ctx, base);
        return;
    }
    t_videoQuad = true;
    t_videoVertices = 0;
    ORIGINAL(JADE_VIDEO_QUAD)(ctx, base);
    t_videoQuad = false;
    if (t_videoVertices == 0)
        return;
    for (uint32_t i = 0; i < jade::video_quad::kVertices; i++)
    {
        uint32_t x = t_videoVertices + jade::video_quad::kStride * i;
        StoreFloat(x, LoadFloat(x) / stretch);
    }
}

// Room for a geometry's vertices in the frame's vertex memory (82101C00; returns its address).
// Hooked to tell JADE_VIDEO_QUAD's hook where the quad it asked for is.
PPC_FUNC(JADE_VERTEX_ALLOCATE)
{
    ORIGINAL(JADE_VERTEX_ALLOCATE)(ctx, base);
    if (t_videoQuad)
        t_videoVertices = ctx.r3.u32;
}

// The submission of the draw record being filled (820E7D40; r3 the packet): every draw the engine
// asks for ends here. Hooked to mark the records of a movie drawn in a frame, whose art runs past it.
PPC_FUNC(JADE_RECORD_SUBMIT)
{
    uint32_t record = t_item.clipped ? mem::Load32(ctx.r3.u32 + jade::packet::kCurrentRecord) : 0;
    Marks* marks = record != 0 ? MarksOf(ctx.r3.u32) : nullptr;
    if (marks != nullptr)
    {
        if (!marks->ranges.empty() && marks->ranges.back().end == record && marks->ranges.back().clip == t_item.clip)
            marks->ranges.back().end += jade::packet::kRecordSize;
        else
            marks->ranges.push_back({ record, record + jade::packet::kRecordSize, t_item.clip });
    }
    ORIGINAL(JADE_RECORD_SUBMIT)(ctx, base);
}

// The device's draw of a record (821024A8; r3 the packet, r4 the record), on the render thread: the
// one place the engine's draws reach the device from. Hooked to clip the draws of the marked records.
PPC_FUNC(JADE_RECORD_DRAW)
{
    const Marks* marks = MarksOf(ctx.r3.u32);
    if (marks != nullptr)
    {
        uint32_t record = ctx.r4.u32;
        for (const Marks::Range& range : marks->ranges)
        {
            if (record < range.begin || record >= range.end)
                continue;
            gpu::SetDrawClip(range.clip);
            ORIGINAL(JADE_RECORD_DRAW)(ctx, base);
            gpu::SetDrawClip({});
            return;
        }
    }
    ORIGINAL(JADE_RECORD_DRAW)(ctx, base);
}
