#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>

using namespace std;

// ============================================================
// CONFIG
// ============================================================

static constexpr int MAP_W = 48;
static constexpr int MAP_H = 34;
static constexpr int TILE = 24;

static constexpr int START_ENERGY = 100;
static constexpr int START_HP = 100;
static constexpr int MAX_BUGS = 8;

static constexpr double MOVE_COOLDOWN = 0.12;

// ============================================================
// DATA
// ============================================================

struct Tile {
    int type;
    int revealed;
};

struct Player {
    int x;
    int y;
    int hp;
    int energy;
    int score;
    int copper;
    int crystals;
    int depth;
};

struct Bug {
    float x;
    float y;
    float speed;
};

static vector<Tile> world;
static vector<Bug> bugs;

static Player player{};

static int bestScore = 0;

static bool gameOver = false;
static bool paused = false;

static double lastMove = 0.0;
static double lastBugSpawn = 0.0;

static mt19937 rng(
    static_cast<unsigned int>(time(nullptr))
);

// ============================================================
// RANDOM
// ============================================================

static int randomInt(int a, int b) {
    uniform_int_distribution<int> d(a, b);
    return d(rng);
}

static double randomDouble(double a, double b) {
    uniform_real_distribution<double> d(a, b);
    return d(rng);
}

// ============================================================
// MAP HELPERS
// ============================================================

static int indexOf(int x, int y) {
    return y * MAP_W + x;
}

static bool insideMap(int x, int y) {
    return
        x >= 0 &&
        x < MAP_W &&
        y >= 0 &&
        y < MAP_H;
}

// ============================================================
// JAVASCRIPT BRIDGE
// ============================================================

EM_JS(void, js_setup, (), {
    window.pixelMiner = window.pixelMiner || {};

    const canvas =
        document.getElementById("gameCanvas") ||
        document.querySelector("canvas");

    if (!canvas) {
        return;
    }

    window.pixelMiner.canvas = canvas;
    window.pixelMiner.ctx = canvas.getContext("2d");

    window.pixelMiner.keys = {};
    window.pixelMiner.mouseDown = false;
    window.pixelMiner.mouseX = 0;
    window.pixelMiner.mouseY = 0;

    canvas.tabIndex = 0;
    canvas.focus();

    canvas.addEventListener("keydown", function(e) {
        window.pixelMiner.keys[e.key] = true;

        if (
            e.key === "ArrowUp" ||
            e.key === "ArrowDown" ||
            e.key === "ArrowLeft" ||
            e.key === "ArrowRight" ||
            e.key === " "
        ) {
            e.preventDefault();
        }
    });

    canvas.addEventListener("keyup", function(e) {
        window.pixelMiner.keys[e.key] = false;
    });

    canvas.addEventListener("mousedown", function(e) {
        const r = canvas.getBoundingClientRect();

        window.pixelMiner.mouseX =
            (e.clientX - r.left) *
            canvas.width / r.width;

        window.pixelMiner.mouseY =
            (e.clientY - r.top) *
            canvas.height / r.height;

        window.pixelMiner.mouseDown = true;
    });

    canvas.addEventListener("mousemove", function(e) {
        const r = canvas.getBoundingClientRect();

        window.pixelMiner.mouseX =
            (e.clientX - r.left) *
            canvas.width / r.width;

        window.pixelMiner.mouseY =
            (e.clientY - r.top) *
            canvas.height / r.height;
    });

    window.addEventListener("mouseup", function() {
        window.pixelMiner.mouseDown = false;
    });

    canvas.addEventListener(
        "touchstart",
        function(e) {
            const t = e.touches[0];

            if (!t) return;

            const r = canvas.getBoundingClientRect();

            window.pixelMiner.mouseX =
                (t.clientX - r.left) *
                canvas.width / r.width;

            window.pixelMiner.mouseY =
                (t.clientY - r.top) *
                canvas.height / r.height;

            window.pixelMiner.mouseDown = true;

            e.preventDefault();
        },
        { passive: false }
    );

    canvas.addEventListener(
        "touchmove",
        function(e) {
            const t = e.touches[0];

            if (!t) return;

            const r = canvas.getBoundingClientRect();

            window.pixelMiner.mouseX =
                (t.clientX - r.left) *
                canvas.width / r.width;

            window.pixelMiner.mouseY =
                (t.clientY - r.top) *
                canvas.height / r.height;

            e.preventDefault();
        },
        { passive: false }
    );

    canvas.addEventListener(
        "touchend",
        function(e) {
            window.pixelMiner.mouseDown = false;
            e.preventDefault();
        },
        { passive: false }
    );
});

EM_JS(int, js_key, (int key), {
    if (
        !window.pixelMiner ||
        !window.pixelMiner.keys
    ) {
        return 0;
    }

    if (key === 0) {
        return window.pixelMiner.keys["ArrowUp"] ||
               window.pixelMiner.keys["w"] ||
               window.pixelMiner.keys["W"] ? 1 : 0;
    }

    if (key === 1) {
        return window.pixelMiner.keys["ArrowDown"] ||
               window.pixelMiner.keys["s"] ||
               window.pixelMiner.keys["S"] ? 1 : 0;
    }

    if (key === 2) {
        return window.pixelMiner.keys["ArrowLeft"] ||
               window.pixelMiner.keys["a"] ||
               window.pixelMiner.keys["A"] ? 1 : 0;
    }

    if (key === 3) {
        return window.pixelMiner.keys["ArrowRight"] ||
               window.pixelMiner.keys["d"] ||
               window.pixelMiner.keys["D"] ? 1 : 0;
    }

    return 0;
});

EM_JS(int, js_mouse_down, (), {
    return
        window.pixelMiner &&
        window.pixelMiner.mouseDown
        ? 1
        : 0;
});

EM_JS(int, js_mouse_x, (), {
    return window.pixelMiner
        ? window.pixelMiner.mouseX
        : 0;
});

EM_JS(int, js_mouse_y, (), {
    return window.pixelMiner
        ? window.pixelMiner.mouseY
        : 0;
});

EM_JS(void, js_hud, (
    int score,
    int best,
    int hp,
    int energy,
    int copper,
    int crystals,
    int depth
), {
    function setText(id, value) {
        const e = document.getElementById(id);
        if (e) {
            e.textContent = String(value);
        }
    }

    setText("score", score);
    setText("bestScore", best);
    setText("hp", hp);
    setText("energy", energy);
    setText("copper", copper);
    setText("crystals", crystals);
    setText("depth", depth);

    const status =
        document.getElementById("gameStatus") ||
        document.getElementById("status");

    if (status) {
        status.textContent =
            hp <= 0 || energy <= 0
                ? "GAME OVER"
                : "MINING";
    }
});

EM_JS(void, js_message, (const char* ptr), {
    const message = UTF8ToString(ptr);

    const e =
        document.getElementById("message");

    if (e) {
        e.textContent = message;
    }
});

EM_JS(void, js_save_best, (int score), {
    try {
        localStorage.setItem(
            "pixel_miner_best",
            String(score)
        );
    } catch (e) {}
});

EM_JS(int, js_load_best, (), {
    try {
        const value =
            localStorage.getItem("pixel_miner_best");

        if (!value) {
            return 0;
        }

        const n =
            parseInt(value, 10);

        return Number.isFinite(n)
            ? n
            : 0;
    } catch (e) {
        return 0;
    }
});


// ============================================================
// SIMPLE SAFE RENDERER
// ============================================================

EM_JS(void, js_render, (
    int mapW,
    int mapH,
    int tile,
    int worldPtr,
    int playerX,
    int playerY,
    int bugCount,
    int bugsPtr,
    int depth
), {
    if (
        !window.pixelMiner ||
        !window.pixelMiner.ctx
    ) {
        return;
    }

    const ctx =
        window.pixelMiner.ctx;

    const canvas =
        window.pixelMiner.canvas;

    const heap =
        HEAP32;

    const base =
        worldPtr >> 2;

    const worldWidth =
        mapW * tile;

    const worldHeight =
        mapH * tile;

    let cameraX =
        playerX * tile +
        tile / 2 -
        canvas.width / 2;

    let cameraY =
        playerY * tile +
        tile / 2 -
        canvas.height / 2;

    cameraX = Math.max(
        0,
        Math.min(
            cameraX,
            Math.max(
                0,
                worldWidth - canvas.width
            )
        )
    );

    cameraY = Math.max(
        0,
        Math.min(
            cameraY,
            Math.max(
                0,
                worldHeight - canvas.height
            )
        )
    );

    ctx.clearRect(
        0,
        0,
        canvas.width,
        canvas.height
    );

    ctx.fillStyle =
        "#02070c";

    ctx.fillRect(
        0,
        0,
        canvas.width,
        canvas.height
    );

    ctx.save();

    ctx.translate(
        -cameraX,
        -cameraY
    );

    // Background
    ctx.fillStyle =
        depth % 2 === 0
            ? "#0b151d"
            : "#071019";

    ctx.fillRect(
        0,
        0,
        worldWidth,
        worldHeight
    );

    const startX =
        Math.max(
            0,
            Math.floor(cameraX / tile) - 1
        );

    const startY =
        Math.max(
            0,
            Math.floor(cameraY / tile) - 1
        );

    const endX =
        Math.min(
            mapW - 1,
            Math.ceil(
                (cameraX + canvas.width) / tile
            ) + 1
        );

    const endY =
        Math.min(
            mapH - 1,
            Math.ceil(
                (cameraY + canvas.height) / tile
            ) + 1
        );

    // ========================================================
    // TILES
    // ========================================================

    for (
        let y = startY;
        y <= endY;
        y++
    ) {
        for (
            let x = startX;
            x <= endX;
            x++
        ) {

            const offset =
                base +
                (y * mapW + x) * 2;

            const type =
                heap[offset];

            const revealed =
                heap[offset + 1];

            const sx =
                x * tile;

            const sy =
                y * tile;

            if (!revealed) {

                ctx.fillStyle =
                    "#020509";

                ctx.fillRect(
                    sx,
                    sy,
                    tile - 1,
                    tile - 1
                );

                continue;
            }

            if (type === 0) {
                ctx.fillStyle =
                    "#1c2d39";
            }
            else if (type === 1) {
                ctx.fillStyle =
                    "#3a4852";
            }
            else if (type === 2) {
                ctx.fillStyle =
                    "#9b6338";
            }
            else if (type === 3) {
                ctx.fillStyle =
                    "#876ce0";
            }
            else if (type === 4) {
                ctx.fillStyle =
                    "#277e69";
            }
            else if (type === 5) {
                ctx.fillStyle =
                    "#157e67";
            }
            else if (type === 6) {
                ctx.fillStyle =
                    "#693b4a";
            }
            else {
                ctx.fillStyle =
                    "#18252e";
            }

            ctx.fillRect(
                sx,
                sy,
                tile - 1,
                tile - 1
            );

            // Rock
            if (type === 1) {

                ctx.fillStyle =
                    "rgba(255,255,255,0.09)";

                ctx.fillRect(
                    sx + 4,
                    sy + 5,
                    6,
                    3
                );

                ctx.fillRect(
                    sx + 14,
                    sy + 14,
                    5,
                    3
                );
            }

            // Copper
            if (type === 2) {

                ctx.fillStyle =
                    "#ffd09b";

                ctx.fillRect(
                    sx + 7,
                    sy + 7,
                    10,
                    10
                );
            }

            // Crystal
            if (type === 3) {

                ctx.fillStyle =
                    "#eee5ff";

                ctx.beginPath();

                ctx.moveTo(
                    sx + 12,
                    sy + 4
                );

                ctx.lineTo(
                    sx + 19,
                    sy + 12
                );

                ctx.lineTo(
                    sx + 12,
                    sy + 20
                );

                ctx.lineTo(
                    sx + 5,
                    sy + 12
                );

                ctx.closePath();

                ctx.fill();
            }

            // Energy
            if (type === 4) {

                ctx.fillStyle =
                    "#c6fff1";

                ctx.beginPath();

                ctx.arc(
                    sx + 12,
                    sy + 12,
                    6,
                    0,
                    Math.PI * 2
                );

                ctx.fill();
            }

            // Exit
            if (type === 5) {

                ctx.strokeStyle =
                    "#7fffd5";

                ctx.lineWidth = 2;

                ctx.beginPath();

                ctx.arc(
                    sx + 12,
                    sy + 12,
                    7,
                    0,
                    Math.PI * 2
                );

                ctx.stroke();

                ctx.fillStyle =
                    "#d7fff4";

                ctx.font =
                    "bold 12px system-ui";

                ctx.textAlign =
                    "center";

                ctx.fillText(
                    "↓",
                    sx + 12,
                    sy + 16
                );
            }

            // Hazard
            if (type === 6) {

                ctx.fillStyle =
                    "#ff7f9d";

                ctx.beginPath();

                ctx.arc(
                    sx + 12,
                    sy + 12,
                    5,
                    0,
                    Math.PI * 2
                );

                ctx.fill();
            }
        }
    }

    // ========================================================
    // BUGS
    // ========================================================

    if (
        bugCount > 0 &&
        bugsPtr
    ) {

        const bugsHeap =
            HEAPF32;

        const bugBase =
            bugsPtr >> 2;

        for (
            let i = 0;
            i < bugCount;
            i++
        ) {

            const bx =
                bugsHeap[
                    bugBase + i * 3
                ];

            const by =
                bugsHeap[
                    bugBase + i * 3 + 1
                ];

            const sx =
                bx * tile;

            const sy =
                by * tile;

            ctx.fillStyle =
                "#ff527a";

            ctx.beginPath();

            ctx.arc(
                sx + 12,
                sy + 12,
                8,
                0,
                Math.PI * 2
            );

            ctx.fill();

            ctx.fillStyle =
                "#ffffff";

            ctx.fillRect(
                sx + 8,
                sy + 9,
                2,
                2
            );

            ctx.fillRect(
                sx + 14,
                sy + 9,
                2,
                2
            );
        }
    }

    // ========================================================
    // PLAYER
    // ========================================================

    const px =
        playerX * tile + 12;

    const py =
        playerY * tile + 12;

    ctx.fillStyle =
        "#5ff6ff";

    ctx.beginPath();

    ctx.arc(
        px,
        py,
        9,
        0,
        Math.PI * 2
    );

    ctx.fill();

    ctx.fillStyle =
        "#ffffff";

    ctx.beginPath();

    ctx.arc(
        px - 3,
        py - 3,
        2,
        0,
        Math.PI * 2
    );

    ctx.arc(
        px + 3,
        py - 3,
        2,
        0,
        Math.PI * 2
    );

    ctx.fill();

    ctx.restore();

    // ========================================================
    // DEPTH LABEL
    // ========================================================

    ctx.fillStyle =
        "rgba(0,0,0,0.5)";

    ctx.fillRect(
        12,
        12,
        125,
        32
    );

    ctx.fillStyle =
        "#bde9f2";

    ctx.font =
        "bold 14px system-ui";

    ctx.textAlign =
        "left";

    ctx.fillText(
        "DEPTH " + depth,
        22,
        33
    );
});

// ============================================================
// WORLD GENERATION
// ============================================================

static void generateWorld() {

    world.clear();

    world.resize(
        MAP_W * MAP_H
    );

    const depth =
        player.depth;

    for (
        int y = 0;
        y < MAP_H;
        y++
    ) {
        for (
            int x = 0;
            x < MAP_W;
            x++
        ) {

            Tile tile;

            tile.revealed = 0;

            int roll =
                randomInt(0, 99);

            int rockChance =
                min(
                    32,
                    22 + depth * 2
                );

            if (roll < rockChance) {
                tile.type = 1;
            }
            else if (roll < 78) {
                tile.type = 0;
            }
            else if (roll < 91) {
                tile.type = 2;
            }
            else if (roll < 97) {
                tile.type = 3;
            }
            else {
                tile.type = 4;
            }

            world[
                indexOf(x, y)
            ] = tile;
        }
    }

    // Safe starting area
    const sx = MAP_W / 2;
    const sy = MAP_H / 2;

    for (
        int dy = -2;
        dy <= 2;
        dy++
    ) {
        for (
            int dx = -2;
            dx <= 2;
            dx++
        ) {

            const int x =
                sx + dx;

            const int y =
                sy + dy;

            if (!insideMap(x, y)) {
                continue;
            }

            world[
                indexOf(x, y)
            ].type = 0;

            world[
                indexOf(x, y)
            ].revealed = 1;
        }
    }

    // Exit
    int exitX;
    int exitY;

    do {
        exitX =
            randomInt(3, MAP_W - 4);

        exitY =
            randomInt(3, MAP_H - 4);
    }
    while (
        abs(exitX - sx) +
        abs(exitY - sy) < 12
    );

    world[
        indexOf(exitX, exitY)
    ].type = 5;

    world[
        indexOf(exitX, exitY)
    ].revealed = 1;

    // Hazards
    const int hazardCount =
        min(
            10,
            3 + player.depth
        );

    for (
        int i = 0;
        i < hazardCount;
        i++
    ) {

        int hx =
            randomInt(2, MAP_W - 3);

        int hy =
            randomInt(2, MAP_H - 3);

        if (
            abs(hx - sx) +
            abs(hy - sy) < 7
        ) {
            continue;
        }

        world[
            indexOf(hx, hy)
        ].type = 6;
    }
}

// ============================================================
// REVEAL
// ============================================================

static void revealAround(
    int cx,
    int cy
) {

    for (
        int dy = -2;
        dy <= 2;
        dy++
    ) {
        for (
            int dx = -2;
            dx <= 2;
            dx++
        ) {

            if (
                abs(dx) +
                abs(dy) > 2
            ) {
                continue;
            }

            const int x =
                cx + dx;

            const int y =
                cy + dy;

            if (
                insideMap(x, y)
            ) {
                world[
                    indexOf(x, y)
                ].revealed = 1;
            }
        }
    }
}

// ============================================================
// MINING
// ============================================================

static void mineTile(
    int x,
    int y
) {

    if (!insideMap(x, y)) {
        return;
    }

    Tile& tile =
        world[indexOf(x, y)];

    tile.revealed = 1;

    if (tile.type == 1) {

        tile.type = 0;

        player.score +=
            1 + player.depth;

        player.energy -= 2;
    }

    else if (tile.type == 2) {

        tile.type = 0;

        player.copper++;

        player.score +=
            10 + player.depth * 2;

        player.energy -= 2;
    }

    else if (tile.type == 3) {

        tile.type = 0;

        player.crystals++;

        player.score +=
            25 + player.depth * 3;

        player.energy -= 2;
    }

    else if (tile.type == 4) {

        tile.type = 0;

        player.score += 15;

        player.energy += 20;

        player.energy =
            min(
                player.energy,
                START_ENERGY
            );
    }

    else if (tile.type == 6) {

        tile.type = 0;

        player.hp -=
            12;

        player.score += 3;
    }
}

// ============================================================
// MOVE
// ============================================================

static void tryMove(
    int dx,
    int dy
) {

    if (
        gameOver ||
        paused
    ) {
        return;
    }

    if (
        player.energy <= 0
    ) {

        player.energy = 0;

        gameOver = true;

        js_message(
            "OUT OF ENERGY"
        );

        return;
    }

    const int nx =
        player.x + dx;

    const int ny =
        player.y + dy;

    if (
        !insideMap(nx, ny)
    ) {
        return;
    }

    Tile& target =
        world[indexOf(nx, ny)];

    // Rock: mine it instead of getting stuck
    if (target.type == 1) {

        mineTile(
            nx,
            ny
        );

        revealAround(
            player.x,
            player.y
        );

        return;
    }

    // Move
    player.x = nx;
    player.y = ny;

    player.energy--;

    revealAround(
        player.x,
        player.y
    );

    // Resource
    if (
        target.type == 2 ||
        target.type == 3 ||
        target.type == 4 ||
        target.type == 6
    ) {

        mineTile(
            nx,
            ny
        );
    }

    // Exit
    if (
        target.type == 5
    ) {

        player.depth++;

        player.score +=
            100 * player.depth;

        player.energy =
            min(
                START_ENERGY,
                player.energy + 30
            );

        player.hp =
            min(
                START_HP,
                player.hp + 20
            );

        bugs.clear();

        generateWorld();

        revealAround(
            player.x,
            player.y
        );

        js_message(
            "DESCENDED TO DEPTH " +
            to_string(player.depth)
                .c_str()
        );
    }

    if (
        player.energy <= 0 ||
        player.hp <= 0
    ) {

        player.energy =
            max(0, player.energy);

        player.hp =
            max(0, player.hp);

        gameOver = true;

        js_message(
            "RUN OVER"
        );
    }
}

// ============================================================
// BUGS
// ============================================================

static void spawnBug() {

    if (
        static_cast<int>(
            bugs.size()
        ) >=
        min(
            MAX_BUGS,
            3 + player.depth
        )
    ) {
        return;
    }

    Bug bug{};

    bool valid = false;

    for (
        int attempt = 0;
        attempt < 50;
        attempt++
    ) {

        bug.x =
            static_cast<float>(
                randomDouble(
                    2.0,
                    MAP_W - 3.0
                )
            );

        bug.y =
            static_cast<float>(
                randomDouble(
                    2.0,
                    MAP_H - 3.0
                )
            );

        const double d =
            hypot(
                bug.x -
                    player.x,
                bug.y -
                    player.y
            );

        if (d > 8.0) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        return;
    }

    bug.speed =
        static_cast<float>(
            randomDouble(
                0.25,
                0.55 +
                player.depth * 0.03
            )
        );

    bugs.push_back(
        bug
    );
}

static void updateBugs(
    double dt
) {

    if (
        gameOver ||
        paused
    ) {
        return;
    }

    for (
        Bug& bug :
        bugs
    ) {

        const double dx =
            player.x -
            bug.x;

        const double dy =
            player.y -
            bug.y;

        const double d =
            sqrt(
                dx * dx +
                dy * dy
            );

        if (d > 0.05) {

            bug.x +=
                static_cast<float>(
                    dx / d *
                    bug.speed *
                    dt
                );

            bug.y +=
                static_cast<float>(
                    dy / d *
                    bug.speed *
                    dt
                );
        }

        if (d < 0.7) {

            player.hp -=
                8 + player.depth;

            bug.x =
                static_cast<float>(
                    randomDouble(
                        2,
                        MAP_W - 3
                    )
                );

            bug.y =
                static_cast<float>(
                    randomDouble(
                        2,
                        MAP_H - 3
                    )
                );

            if (
                player.hp <= 0
            ) {

                player.hp = 0;

                gameOver = true;

                js_message(
                    "CAUGHT BY A CAVE CRAWLER"
                );
            }
        }
    }
}

// ============================================================
// INPUT
// ============================================================

static void keyboardInput() {

    if (
        js_key(0)
    ) {
        tryMove(0, -1);
    }
    else if (
        js_key(1)
    ) {
        tryMove(0, 1);
    }
    else if (
        js_key(2)
    ) {
        tryMove(-1, 0);
    }
    else if (
        js_key(3)
    ) {
        tryMove(1, 0);
    }
}

static void mouseInput() {

    if (!js_mouse_down()) {
        return;
    }

    const int mx =
        js_mouse_x();

    const int my =
        js_mouse_y();

    const int tx =
        mx / TILE;

    const int ty =
        my / TILE;

    if (
        !insideMap(tx, ty)
    ) {
        return;
    }

    const int dx =
        tx - player.x;

    const int dy =
        ty - player.y;

    if (
        abs(dx) +
        abs(dy) != 1
    ) {
        return;
    }

    if (dx > 0) {
        tryMove(1, 0);
    }
    else if (dx < 0) {
        tryMove(-1, 0);
    }
    else if (dy > 0) {
        tryMove(0, 1);
    }
    else {
        tryMove(0, -1);
    }
}

// ============================================================
// RENDER
// ============================================================

static void render() {

    if (world.empty()) {
        return;
    }

    js_render(
        MAP_W,
        MAP_H,
        TILE,
        static_cast<int>(
            reinterpret_cast<uintptr_t>(
                world.data()
            )
        ),
        player.x,
        player.y,
        static_cast<int>(
            bugs.size()
        ),
        bugs.empty()
            ? 0
            : static_cast<int>(
                reinterpret_cast<uintptr_t>(
                    bugs.data()
                )
            ),
        player.depth
    );
}

// ============================================================
// HUD
// ============================================================

static void updateHUD() {

    js_hud(
        player.score,
        bestScore,
        player.hp,
        player.energy,
        player.copper,
        player.crystals,
        player.depth
    );
}

// ============================================================
// RESTART
// ============================================================

extern "C"
EMSCRIPTEN_KEEPALIVE
void restart_game() {

    player.x =
        MAP_W / 2;

    player.y =
        MAP_H / 2;

    player.hp =
        START_HP;

    player.energy =
        START_ENERGY;

    player.score = 0;

    player.copper = 0;

    player.crystals = 0;

    player.depth = 1;

    gameOver = false;
    paused = false;

    bugs.clear();

    lastMove = 0.0;
    lastBugSpawn = 0.0;

    generateWorld();

    revealAround(
        player.x,
        player.y
    );

    js_message(
        "MINING"
    );

    updateHUD();

    render();
}

// ============================================================
// MAIN LOOP
// ============================================================

static void tick(
    void*
) {

    static double previous =
        0.0;

    const double now =
        emscripten_get_now()
        / 1000.0;

    double dt =
        now - previous;

    previous = now;

    if (
        dt < 0.0
    ) {
        dt = 0.0;
    }

    if (
        dt > 0.1
    ) {
        dt = 0.1;
    }

    if (
        !gameOver &&
        !paused
    ) {

        if (
            now - lastMove
            >= MOVE_COOLDOWN
        ) {

            keyboardInput();

            mouseInput();

            lastMove =
                now;
        }

        if (
            now - lastBugSpawn
            >= max(
                2.5,
                6.0 -
                player.depth * 0.2
            )
        ) {

            spawnBug();

            lastBugSpawn =
                now;
        }

        updateBugs(
            dt
        );

        if (
            player.score >
            bestScore
        ) {

            bestScore =
                player.score;

            js_save_best(
                bestScore
            );
        }
    }

    updateHUD();

    render();
}

// ============================================================
// MAIN
// ============================================================

int main() {

    js_setup();

    bestScore =
        js_load_best();

    player.x =
        MAP_W / 2;

    player.y =
        MAP_H / 2;

    player.hp =
        START_HP;

    player.energy =
        START_ENERGY;

    player.score = 0;

    player.copper = 0;

    player.crystals = 0;

    player.depth = 1;

    gameOver = false;
    paused = false;

    generateWorld();

    revealAround(
        player.x,
        player.y
    );

    js_message(
        "MINING"
    );

    updateHUD();

    render();

    emscripten_set_main_loop_arg(
        tick,
        nullptr,
        0,
        1
    );

    return 0;
}
