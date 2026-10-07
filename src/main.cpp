#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace std;

static constexpr int MAP_W = 60;
static constexpr int MAP_H = 40;
static constexpr int TILE = 24;
static constexpr int CANVAS_W = 960;
static constexpr int CANVAS_H = 600;

static constexpr int MAX_HP = 100;
static constexpr int MAX_ENERGY = 100;

enum TileType {
    FLOOR = 0,
    ROCK = 1,
    COPPER = 2,
    CRYSTAL = 3,
    ENERGY = 4,
    EXIT_TILE = 5,
    HAZARD = 6
};

struct Tile {
    int type;
    int revealed;
};

struct Bug {
    float x;
    float y;
    float speed;
};

struct Player {
    int x = MAP_W / 2;
    int y = MAP_H / 2;
    int hp = MAX_HP;
    int energy = MAX_ENERGY;
    int score = 0;
    int copper = 0;
    int crystals = 0;
    int depth = 1;
};

static vector<Tile> world;
static vector<Bug> bugs;
static Player player;

static mt19937 rng(0xC0FFEEu);

static int bestScore = 0;
static bool paused = false;
static bool gameOver = false;

static double lastMove = 0.0;
static double lastSpawn = 0.0;
static double lastDamage = -100.0;

static int exitX = MAP_W - 4;
static int exitY = MAP_H - 4;

static int idx(int x, int y) {
    return y * MAP_W + x;
}

static bool inside(int x, int y) {
    return x >= 0 && x < MAP_W && y >= 0 && y < MAP_H;
}

static int rnd(int a, int b) {
    uniform_int_distribution<int> d(a, b);
    return d(rng);
}

static float rndf(float a, float b) {
    uniform_real_distribution<float> d(a, b);
    return d(rng);
}

// ------------------------------------------------------------
// Browser bridge
// ------------------------------------------------------------

EM_JS(void, js_init, (), {
    window.pixelMiner = window.pixelMiner || {};
    const c = document.getElementById("gameCanvas");
    if (!c) return;

    window.pixelMiner.canvas = c;
    window.pixelMiner.ctx = c.getContext("2d");
    window.pixelMiner.keys = {};
    window.pixelMiner.mouseX = 0;
    window.pixelMiner.mouseY = 0;
    window.pixelMiner.mouseDown = false;

    c.setAttribute("tabindex", "0");
    c.focus();

    const keyMap = {
        ArrowUp: "up", w: "up",
        ArrowDown: "down", s: "down",
        ArrowLeft: "left", a: "left",
        ArrowRight: "right", d: "right"
    };

    c.addEventListener("keydown", e => {
        const k = keyMap[e.key];
        if (k) {
            window.pixelMiner.keys[k] = true;
            e.preventDefault();
        }
        if (e.key === " " || e.key === "p" || e.key === "P") {
            if (typeof Module !== "undefined" && Module.ccall) {
                Module.ccall("toggle_pause", null, [], []);
            }
            e.preventDefault();
        }
    });

    c.addEventListener("keyup", e => {
        const k = keyMap[e.key];
        if (k) {
            window.pixelMiner.keys[k] = false;
            e.preventDefault();
        }
    });

    const pointer = e => {
        const r = c.getBoundingClientRect();
        window.pixelMiner.mouseX =
            (e.clientX - r.left) * c.width / r.width;
        window.pixelMiner.mouseY =
            (e.clientY - r.top) * c.height / r.height;
    };

    c.addEventListener("pointermove", pointer);
    c.addEventListener("pointerdown", e => {
        pointer(e);
        window.pixelMiner.mouseDown = true;
        c.setPointerCapture?.(e.pointerId);
    });
    c.addEventListener("pointerup", e => {
        window.pixelMiner.mouseDown = false;
        try { c.releasePointerCapture(e.pointerId); } catch (_) {}
    });
    c.addEventListener("pointercancel", () => {
        window.pixelMiner.mouseDown = false;
    });
});

EM_JS(int, js_key, (int key), {
    if (!window.pixelMiner || !window.pixelMiner.keys) return 0;
    const names = ["up", "down", "left", "right"];
    return window.pixelMiner.keys[names[key]] ? 1 : 0;
});

EM_JS(int, js_mouse_down, (), {
    return window.pixelMiner && window.pixelMiner.mouseDown ? 1 : 0;
});

EM_JS(int, js_mouse_x, (), {
    return window.pixelMiner ? window.pixelMiner.mouseX : 0;
});

EM_JS(int, js_mouse_y, (), {
    return window.pixelMiner ? window.pixelMiner.mouseY : 0;
});

EM_JS(void, js_hud, (
    int score, int best, int hp, int energy,
    int depth, int copper, int crystals, int pausedState
), {
    const set = (id, value) => {
        const e = document.getElementById(id);
        if (e) e.textContent = String(value);
    };

    set("score", score);
    set("bestScore", best);
    set("hp", hp);
    set("energy", energy);
    set("depth", depth);
    set("copper", copper);
    set("crystals", crystals);

    const hpBar = document.getElementById("hpBar");
    const energyBar = document.getElementById("energyBar");
    if (hpBar) hpBar.style.width = Math.max(0, hp) + "%";
    if (energyBar) energyBar.style.width = Math.max(0, energy) + "%";

    const pause = document.getElementById("pauseBtn");
    if (pause) pause.textContent = pausedState ? "▶ Resume" : "Ⅱ Pause";

    const state = document.getElementById("state");
    if (state) {
        if (pausedState) state.textContent = "PAUSED";
        else state.textContent = "EXPLORING";
    }
});

EM_JS(void, js_notice, (const char* msg), {
    const text = UTF8ToString(msg);
    const box = document.getElementById("notice");
    if (!box) return;

    box.textContent = text;
    box.classList.add("show");

    clearTimeout(window.pixelMinerNoticeTimer);
    window.pixelMinerNoticeTimer =
        setTimeout(() => box.classList.remove("show"), 1500);
});

EM_JS(void, js_save_best, (int score), {
    try {
        localStorage.setItem("pixel_miner_best", String(score));
    } catch (_) {}
});

EM_JS(int, js_load_best, (), {
    try {
        const n = parseInt(
            localStorage.getItem("pixel_miner_best") || "0",
            10
        );
        return Number.isFinite(n) ? n : 0;
    } catch (_) {
        return 0;
    }
});

// ------------------------------------------------------------
// Renderer with camera follow
// ------------------------------------------------------------

EM_JS(void, js_render, (
    int mapW, int mapH, int tile,
    int worldPtr, int playerX, int playerY,
    int bugCount, int bugsPtr,
    int depth
), {
    if (!window.pixelMiner || !window.pixelMiner.ctx) return;

    const ctx = window.pixelMiner.ctx;
    const canvas = window.pixelMiner.canvas;
    const heap = HEAP32;
    const base = worldPtr >> 2;

    const worldW = mapW * tile;
    const worldH = mapH * tile;

    // Camera follows player, preventing the "stuck in center" feeling.
    let camX = playerX * tile + tile / 2 - canvas.width / 2;
    let camY = playerY * tile + tile / 2 - canvas.height / 2;

    camX = Math.max(0, Math.min(camX, worldW - canvas.width));
    camY = Math.max(0, Math.min(camY, worldH - canvas.height));

    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#02070c";
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    ctx.save();
    ctx.translate(-camX, -camY);

    // Subtle depth background.
    ctx.fillStyle = depth % 2 ? "#07131d" : "#0b111b";
    ctx.fillRect(0, 0, worldW, worldH);

    const startX = Math.max(0, Math.floor(camX / tile) - 1);
    const startY = Math.max(0, Math.floor(camY / tile) - 1);
    const endX = Math.min(mapW - 1, Math.ceil((camX + canvas.width) / tile) + 1);
    const endY = Math.min(mapH - 1, Math.ceil((camY + canvas.height) / tile) + 1);

    for (let y = startY; y <= endY; y++) {
        for (let x = startX; x <= endX; x++) {
            const off = base + (y * mapW + x) * 2;
            const type = heap[off];
            const revealed = heap[off + 1];

            const sx = x * tile;
            const sy = y * tile;

            if (!revealed) {
                ctx.fillStyle = "#02060b";
                ctx.fillRect(sx, sy, tile - 1, tile - 1);
                continue;
            }

            if (type === 0) ctx.fillStyle = "#1c2d39";
            else if (type === 1) ctx.fillStyle = "#33434d";
            else if (type === 2) ctx.fillStyle = "#a86d3d";
            else if (type === 3) ctx.fillStyle = "#9878ff";
            else if (type === 4) ctx.fillStyle = "#2eaa8b";
            else if (type === 5) ctx.fillStyle = "#1b8d72";
            else if (type === 6) ctx.fillStyle = "#5b3d47";
            else ctx.fillStyle = "#17242d";

            ctx.fillRect(sx, sy, tile - 1, tile - 1);

            // Tile grid
            ctx.strokeStyle = "rgba(120,180,210,.055)";
            ctx.strokeRect(sx, sy, tile - 1, tile - 1);

            if (type === 1) {
                ctx.fillStyle = "rgba(255,255,255,.08)";
                ctx.fillRect(sx + 4, sy + 5, 6, 3);
                ctx.fillRect(sx + 13, sy + 14, 5, 3);
            }

            if (type === 2) {
                ctx.fillStyle = "#ffd09b";
                ctx.fillRect(sx + 7, sy + 7, 10, 10);
            }

            if (type === 3) {
                ctx.fillStyle = "#efe7ff";
                ctx.beginPath();
                ctx.moveTo(sx + 12, sy + 4);
                ctx.lineTo(sx + 19, sy + 12);
                ctx.lineTo(sx + 12, sy + 20);
                ctx.lineTo(sx + 5, sy + 12);
                ctx.closePath();
                ctx.fill();
            }

            if (type === 4) {
                ctx.fillStyle = "#c5fff1";
                ctx.beginPath();
                ctx.arc(sx + 12, sy + 12, 6, 0, Math.PI * 2);
                ctx.fill();
            }

            if (type === 5) {
                ctx.strokeStyle = "#7fffd5";
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.arc(sx + 12, sy + 12, 7, 0, Math.PI * 2);
                ctx.stroke();
                ctx.fillStyle = "#d7fff4";
                ctx.font = "bold 9px system-ui";
                ctx.textAlign = "center";
                ctx.fillText("↓", sx + 12, sy + 15);
            }

            if (type === 6) {
                ctx.fillStyle = "#ff8aa4";
                ctx.beginPath();
                ctx.arc(sx + 12, sy + 12, 4, 0, Math.PI * 2);
                ctx.fill();
            }
        }
    }

    // Bugs
    if (bugCount > 0 && bugsPtr) {
        const fh = HEAPF32;
        const b = bugsPtr >> 2;

        for (let i = 0; i < bugCount; i++) {
            const bx = fh[b + i * 3];
            const by = fh[b + i * 3 + 1];
            const sx = bx * tile;
            const sy = by * tile;

            ctx.fillStyle = "#ff527a";
            ctx.beginPath();
            ctx.arc(sx + 12, sy + 12, 8, 0, Math.PI * 2);
            ctx.fill();

            ctx.fillStyle = "#fff";
            ctx.fillRect(sx + 8, sy + 9, 2, 2);
            ctx.fillRect(sx + 14, sy + 9, 2, 2);
        }
    }

    // Player glow
    const px = playerX * tile + 12;
    const py = playerY * tile + 12;

    const glow = ctx.createRadialGradient(px, py, 2, px, py, 24);
    glow.addColorStop(0, "rgba(95,246,255,.35)");
    glow.addColorStop(1, "rgba(95,246,255,0)");
    ctx.fillStyle = glow;
    ctx.fillRect(px - 25, py - 25, 50, 50);

    ctx.fillStyle = "#5ff6ff";
    ctx.beginPath();
    ctx.arc(px, py, 9, 0, Math.PI * 2);
    ctx.fill();

    ctx.fillStyle = "#fff";
    ctx.beginPath();
    ctx.arc(px - 3, py - 3, 2, 0, Math.PI * 2);
    ctx.arc(px + 3, py - 3, 2, 0, Math.PI * 2);
    ctx.fill();

    ctx.restore();

    // Top-left mini depth marker inside canvas.
    ctx.fillStyle = "rgba(0,0,0,.42)";
    ctx.fillRect(12, 12, 130, 30);
    ctx.fillStyle = "#bde9f2";
    ctx.font = "bold 14px system-ui";
    ctx.textAlign = "left";
    ctx.fillText("DEPTH " + depth, 22, 32);
}, 0);

// ------------------------------------------------------------
// World generation
// ------------------------------------------------------------

static void revealAround(int cx, int cy) {
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            if (abs(dx) + abs(dy) <= 3) {
                int x = cx + dx;
                int y = cy + dy;
                if (inside(x, y)) world[idx(x, y)].revealed = 1;
            }
        }
    }
}

static void carvePath(int x1, int y1, int x2, int y2) {
    int x = x1;
    int y = y1;

    while (x != x2 || y != y2) {
        world[idx(x, y)].type = FLOOR;

        if (x != x2 && (y == y2 || rnd(0, 1) == 0)) {
            x += (x2 > x) ? 1 : -1;
        } else if (y != y2) {
            y += (y2 > y) ? 1 : -1;
        }

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (inside(nx, ny)) world[idx(nx, ny)].type = FLOOR;
            }
        }
    }
    world[idx(x2, y2)].type = EXIT_TILE;
}

static void generateWorld() {
    world.assign(MAP_W * MAP_H, Tile{ROCK, 0});
    bugs.clear();

    player.x = MAP_W / 2;
    player.y = MAP_H / 2;

    exitX = MAP_W - 5;
    exitY = MAP_H - 5;

    // Mostly rock, with some walkable cells.
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int r = rnd(0, 99);
            int type = r < 63 ? ROCK : FLOOR;
            if (r >= 63 && r < 69) type = HAZARD;
            world[idx(x, y)] = {type, 0};
        }
    }

    // Guaranteed route from spawn to exit.
    carvePath(player.x, player.y, exitX, exitY);

    // Resources.
    const int resourceCount = 65 + player.depth * 4;
    for (int i = 0; i < resourceCount; ++i) {
        int x = rnd(2, MAP_W - 3);
        int y = rnd(2, MAP_H - 3);

        if (abs(x - player.x) + abs(y - player.y) < 5) continue;
        if (world[idx(x, y)].type != ROCK) continue;

        int r = rnd(0, 99);
        if (r < 65) world[idx(x, y)].type = COPPER;
        else if (r < 92) world[idx(x, y)].type = CRYSTAL;
        else world[idx(x, y)].type = ENERGY;
    }

    // Keep exit safe.
    world[idx(exitX, exitY)].type = EXIT_TILE;

    revealAround(player.x, player.y);

    for (int i = 0; i < min(2 + player.depth / 2, 6); ++i) {
        // Spawn later via timer; this only makes depth scaling explicit.
    }
}

// ------------------------------------------------------------
// Gameplay
// ------------------------------------------------------------

static void gameOverNow(const char* msg) {
    gameOver = true;
    js_notice(msg);
}

static void descend() {
    player.depth++;
    player.score += 100 + player.depth * 20;
    player.energy = min(MAX_ENERGY, player.energy + 35);
    player.hp = min(MAX_HP, player.hp + 20);

    generateWorld();

    string msg = "DEPTH " + to_string(player.depth) + " — KEEP MINING!";
    js_notice(msg.c_str());
}

static void mineTile(int x, int y) {
    if (!inside(x, y)) return;

    Tile& t = world[idx(x, y)];
    t.revealed = 1;

    if (t.type == ROCK) {
        t.type = FLOOR;
        player.energy -= 2;
        player.score += 2;
    } else if (t.type == COPPER) {
        t.type = FLOOR;
        player.energy -= 2;
        player.copper++;
        player.score += 15;
    } else if (t.type == CRYSTAL) {
        t.type = FLOOR;
        player.energy -= 3;
        player.crystals++;
        player.score += 40;
    } else if (t.type == ENERGY) {
        t.type = FLOOR;
        player.energy = min(MAX_ENERGY, player.energy + 25);
        player.score += 20;
        js_notice("ENERGY +25");
    } else if (t.type == HAZARD) {
        t.type = FLOOR;
        player.energy -= 3;
        player.hp -= 10;
        player.score += 5;
        js_notice("Hazard! -10 HP");
    }

    if (player.energy < 0) player.energy = 0;
    if (player.hp < 0) player.hp = 0;
}

static void movePlayer(int dx, int dy) {
    if (paused || gameOver) return;
    if (dx == 0 && dy == 0) return;

    int nx = player.x + dx;
    int ny = player.y + dy;

    if (!inside(nx, ny)) {
        js_notice("Map boundary");
        return;
    }

    Tile& target = world[idx(nx, ny)];

    // Every solid tile can be mined, so movement never permanently traps the player.
    if (target.type == ROCK ||
        target.type == COPPER ||
        target.type == CRYSTAL ||
        target.type == ENERGY ||
        target.type == HAZARD) {
        mineTile(nx, ny);

        if (player.energy <= 0 || player.hp <= 0) {
            gameOverNow(player.hp <= 0 ? "MISSION FAILED" : "OUT OF ENERGY");
        }
        return;
    }

    player.x = nx;
    player.y = ny;

    player.energy = max(0, player.energy - 1);
    revealAround(player.x, player.y);

    if (target.type == EXIT_TILE) {
        descend();
        return;
    }

    if (player.energy <= 0) {
        gameOverNow("OUT OF ENERGY");
    }
}

static void spawnBug() {
    int maxBugs = min(12, 2 + player.depth / 2);
    if ((int)bugs.size() >= maxBugs) return;

    for (int attempt = 0; attempt < 40; ++attempt) {
        float x = (float)rndf(2.0f, (float)MAP_W - 3.0f);
        float y = (float)rndf(2.0f, (float)MAP_H - 3.0f);

        if (hypot(x - player.x, y - player.y) < 9.0) continue;

        int tx = (int)x;
        int ty = (int)y;
        if (!inside(tx, ty)) continue;

        int type = world[idx(tx, ty)].type;
        if (type == ROCK || type == EXIT_TILE) continue;

        bugs.push_back({
            x,
            y,
            rndf(0.45f + player.depth * 0.01f,
                 0.75f + player.depth * 0.015f)
        });
        return;
    }
}

static void updateBugs(double dt) {
    if (paused || gameOver) return;

    for (Bug& b : bugs) {
        float dx = player.x + 0.5f - b.x;
        float dy = player.y + 0.5f - b.y;
        float d = sqrtf(dx * dx + dy * dy);

        if (d > 0.05f) {
            b.x += dx / d * b.speed * (float)dt;
            b.y += dy / d * b.speed * (float)dt;
        }

        if (d < 0.65f) {
            double now = emscripten_get_now() / 1000.0;
            if (now - lastDamage > 0.75) {
                lastDamage = now;
                player.hp -= 8;
                js_notice("Crawler hit! -8 HP");

                if (player.hp <= 0) {
                    player.hp = 0;
                    gameOverNow("DEFEATED BY CRAWLERS");
                }
            }
        }
    }
}

static void handleKeyboard() {
    if (js_key(0)) movePlayer(0, -1);
    else if (js_key(1)) movePlayer(0, 1);
    else if (js_key(2)) movePlayer(-1, 0);
    else if (js_key(3)) movePlayer(1, 0);
}

static void handleMouse() {
    if (!js_mouse_down()) return;

    // Canvas camera math is mirrored here.
    int camX = player.x * TILE + TILE / 2 - CANVAS_W / 2;
    int camY = player.y * TILE + TILE / 2 - CANVAS_H / 2;

    camX = max(0, min(camX, MAP_W * TILE - CANVAS_W));
    camY = max(0, min(camY, MAP_H * TILE - CANVAS_H));

    int tx = (js_mouse_x() + camX) / TILE;
    int ty = (js_mouse_y() + camY) / TILE;

    int dx = tx - player.x;
    int dy = ty - player.y;

    if (abs(dx) + abs(dy) == 1) {
        movePlayer(
            dx > 0 ? 1 : dx < 0 ? -1 : 0,
            dy > 0 ? 1 : dy < 0 ? -1 : 0
        );
    }
}

// ------------------------------------------------------------
// Public controls
// ------------------------------------------------------------

extern "C" {

EMSCRIPTEN_KEEPALIVE
void move_player(int dx, int dy) {
    movePlayer(dx, dy);
}

EMSCRIPTEN_KEEPALIVE
void toggle_pause() {
    if (gameOver) return;
    paused = !paused;
    js_notice(paused ? "GAME PAUSED" : "RESUMED");
}

EMSCRIPTEN_KEEPALIVE
void restart_game() {
    player = Player{};
    player.x = MAP_W / 2;
    player.y = MAP_H / 2;
    paused = false;
    gameOver = false;
    lastMove = 0;
    lastSpawn = 0;
    lastDamage = -100;

    generateWorld();
    js_notice("NEW EXPEDITION");
}

}

// ------------------------------------------------------------
// Tick
// ------------------------------------------------------------

static void tick(void*) {
    static double previous = 0.0;
    double now = emscripten_get_now() / 1000.0;
    double dt = now - previous;
    previous = now;

    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;

    if (!paused && !gameOver) {
        if (now - lastMove >= 0.12) {
            handleKeyboard();
            lastMove = now;
        }

        if (now - lastSpawn >= max(2.5, 5.0 - player.depth * 0.08)) {
            spawnBug();
            lastSpawn = now;
        }

        updateBugs(dt);

        if (player.score > bestScore) {
            bestScore = player.score;
            js_save_best(bestScore);
        }
    }

    js_hud(
        player.score,
        bestScore,
        player.hp,
        player.energy,
        player.depth,
        player.copper,
        player.crystals,
        paused ? 1 : 0
    );

    js_render(
        MAP_W,
        MAP_H,
        TILE,
        (int)(uintptr_t)world.data(),
        player.x,
        player.y,
        (int)bugs.size(),
        bugs.empty() ? 0 : (int)(uintptr_t)bugs.data(),
        player.depth
    );
}

int main() {
    bestScore = js_load_best();

    js_init();

    player = Player{};
    player.x = MAP_W / 2;
    player.y = MAP_H / 2;

    generateWorld();

    js_notice("WASD / ARROWS TO MOVE");

    emscripten_set_main_loop_arg(
        tick,
        nullptr,
        0,
        1
    );

    return 0;
}
