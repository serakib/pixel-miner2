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

static constexpr int START_HP = 100;
static constexpr int START_ENERGY = 100;

static constexpr int ENERGY_REGEN_PER_SECOND = 5;

static constexpr int MAX_BUGS = 8;

static constexpr double MOVE_COOLDOWN = 0.12;

// ============================================================
// TILE TYPES
// ============================================================
// 0 = dirt
// 1 = rock
// 2 = copper
// 3 = crystal
// 4 = energy
// 5 = exit
// 6 = hazard
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

// ============================================================
// GLOBAL STATE
// ============================================================

static vector<Tile> world;
static vector<Bug> bugs;

static Player player{};

static int bestScore = 0;

static bool gameOver = false;
static bool paused = false;

static double lastMoveTime = 0.0;
static double lastBugSpawn = 0.0;
static double lastEnergyRegen = 0.0;

static mt19937 rng(
    static_cast<unsigned int>(time(nullptr))
);

// ============================================================
// RANDOM
// ============================================================

static int randomInt(
    int minValue,
    int maxValue
) {
    uniform_int_distribution<int> distribution(
        minValue,
        maxValue
    );

    return distribution(rng);
}

static double randomDouble(
    double minValue,
    double maxValue
) {
    uniform_real_distribution<double> distribution(
        minValue,
        maxValue
    );

    return distribution(rng);
}

// ============================================================
// MAP HELPERS
// ============================================================

static int indexOf(
    int x,
    int y
) {
    return y * MAP_W + x;
}

static bool insideMap(
    int x,
    int y
) {
    return
        x >= 0 &&
        x < MAP_W &&
        y >= 0 &&
        y < MAP_H;
}

// ============================================================
// JAVASCRIPT SETUP
// ============================================================

EM_JS(
    void,
    js_setup,
    (),
    {
        window.pixelMiner =
            window.pixelMiner || {};

        const canvas =
            document.getElementById("gameCanvas") ||
            document.querySelector("canvas");

        if (!canvas) {
            return;
        }

        window.pixelMiner.canvas =
            canvas;

        window.pixelMiner.ctx =
            canvas.getContext("2d");

        window.pixelMiner.keys = {};

        window.pixelMiner.mouseDown =
            false;

        window.pixelMiner.mouseX = 0;
        window.pixelMiner.mouseY = 0;

        canvas.tabIndex = 0;

        canvas.focus();

        canvas.addEventListener(
            "keydown",
            function(event) {

                window.pixelMiner.keys[
                    event.key
                ] = true;

                if (
                    event.key === "ArrowUp" ||
                    event.key === "ArrowDown" ||
                    event.key === "ArrowLeft" ||
                    event.key === "ArrowRight" ||
                    event.key === " "
                ) {
                    event.preventDefault();
                }
            }
        );

        canvas.addEventListener(
            "keyup",
            function(event) {

                window.pixelMiner.keys[
                    event.key
                ] = false;
            }
        );

        canvas.addEventListener(
            "mousedown",
            function(event) {

                const rect =
                    canvas.getBoundingClientRect();

                window.pixelMiner.mouseX =
                    (event.clientX - rect.left) *
                    canvas.width /
                    rect.width;

                window.pixelMiner.mouseY =
                    (event.clientY - rect.top) *
                    canvas.height /
                    rect.height;

                window.pixelMiner.mouseDown =
                    true;
            }
        );

        window.addEventListener(
            "mouseup",
            function() {

                window.pixelMiner.mouseDown =
                    false;
            }
        );

        canvas.addEventListener(
            "touchstart",
            function(event) {

                const touch =
                    event.touches[0];

                if (!touch) {
                    return;
                }

                const rect =
                    canvas.getBoundingClientRect();

                window.pixelMiner.mouseX =
                    (touch.clientX - rect.left) *
                    canvas.width /
                    rect.width;

                window.pixelMiner.mouseY =
                    (touch.clientY - rect.top) *
                    canvas.height /
                    rect.height;

                window.pixelMiner.mouseDown =
                    true;

                event.preventDefault();
            },
            { passive: false }
        );

        canvas.addEventListener(
            "touchend",
            function(event) {

                window.pixelMiner.mouseDown =
                    false;

                event.preventDefault();
            },
            { passive: false }
        );

        // Allow popup restart to call the exported function.
        window.pixelMiner.restart =
            function() {

                if (
                    window.Module &&
                    typeof window.Module._restart_game ===
                    "function"
                ) {

                    window.Module._restart_game();

                } else {

                    window.location.reload();
                }
            };
    }
);

// ============================================================
// KEY INPUT
// ============================================================

EM_JS(
    int,
    js_key,
    (int key),
    {
        if (
            !window.pixelMiner ||
            !window.pixelMiner.keys
        ) {
            return 0;
        }

        if (key === 0) {

            return (
                window.pixelMiner.keys["ArrowUp"] ||
                window.pixelMiner.keys["w"] ||
                window.pixelMiner.keys["W"]
            ) ? 1 : 0;
        }

        if (key === 1) {

            return (
                window.pixelMiner.keys["ArrowDown"] ||
                window.pixelMiner.keys["s"] ||
                window.pixelMiner.keys["S"]
            ) ? 1 : 0;
        }

        if (key === 2) {

            return (
                window.pixelMiner.keys["ArrowLeft"] ||
                window.pixelMiner.keys["a"] ||
                window.pixelMiner.keys["A"]
            ) ? 1 : 0;
        }

        if (key === 3) {

            return (
                window.pixelMiner.keys["ArrowRight"] ||
                window.pixelMiner.keys["d"] ||
                window.pixelMiner.keys["D"]
            ) ? 1 : 0;
        }

        return 0;
    }
);

// ============================================================
// MOUSE INPUT
// ============================================================

EM_JS(
    int,
    js_mouse_down,
    (),
    {
        if (
            window.pixelMiner &&
            window.pixelMiner.mouseDown
        ) {
            return 1;
        }

        return 0;
    }
);

EM_JS(
    int,
    js_mouse_x,
    (),
    {
        if (window.pixelMiner) {
            return window.pixelMiner.mouseX;
        }

        return 0;
    }
);

EM_JS(
    int,
    js_mouse_y,
    (),
    {
        if (window.pixelMiner) {
            return window.pixelMiner.mouseY;
        }

        return 0;
    }
);

// ============================================================
// HUD
// ============================================================

EM_JS(
    void,
    js_update_hud,
    (
        int score,
        int best,
        int hp,
        int energy,
        int copper,
        int crystals,
        int depth
    ),
    {
        function setText(
            id,
            value
        ) {
            const element =
                document.getElementById(id);

            if (element) {
                element.textContent =
                    String(value);
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
            document.getElementById(
                "gameStatus"
            ) ||
            document.getElementById(
                "status"
            );

        if (status) {

            if (hp <= 0) {
                status.textContent =
                    "GAME OVER";

            } else if (paused) {
                status.textContent =
                    "PAUSED";

            } else if (energy <= 0) {
                status.textContent =
                    "RECHARGING";

            } else {
                status.textContent =
                    "MINING";
            }
        }
    }
);

// ============================================================
// NORMAL MESSAGE
// ============================================================

EM_JS(
    void,
    js_message,
    (const char* messagePtr),
    {
        const message =
            UTF8ToString(
                messagePtr
            );

        const element =
            document.getElementById(
                "message"
            );

        if (element) {
            element.textContent =
                message;
        }
    }
);

// ============================================================
// GAME OVER POPUP
// ============================================================

EM_JS(
    void,
    js_show_game_over,
    (),
    {
        let popup =
            document.getElementById(
                "pixelMinerGameOver"
            );

        if (!popup) {

            popup =
                document.createElement(
                    "div"
                );

            popup.id =
                "pixelMinerGameOver";

            popup.style.position =
                "fixed";

            popup.style.inset =
                "0";

            popup.style.display =
                "flex";

            popup.style.alignItems =
                "center";

            popup.style.justifyContent =
                "center";

            popup.style.background =
                "rgba(0,0,0,0.72)";

            popup.style.zIndex =
                "999999";

            popup.style.fontFamily =
                "system-ui, sans-serif";

            const box =
                document.createElement(
                    "div"
                );

            box.id =
                "pixelMinerGameOverBox";

            box.style.width =
                "min(380px, 86vw)";

            box.style.padding =
                "28px";

            box.style.borderRadius =
                "18px";

            box.style.background =
                "#0b151d";

            box.style.border =
                "1px solid rgba(120,220,255,0.28)";

            box.style.boxShadow =
                "0 20px 70px rgba(0,0,0,0.55)";

            box.style.textAlign =
                "center";

            const title =
                document.createElement(
                    "div"
                );

            title.textContent =
                "GAME OVER";

            title.style.fontSize =
                "32px";

            title.style.fontWeight =
                "800";

            title.style.color =
                "#ffffff";

            title.style.marginBottom =
                "10px";

            const subtitle =
                document.createElement(
                    "div"
                );

            subtitle.textContent =
                "Your mining run has ended.";

            subtitle.style.fontSize =
                "15px";

            subtitle.style.color =
                "#a9c4cf";

            subtitle.style.marginBottom =
                "22px";

            const restart =
                document.createElement(
                    "button"
                );

            restart.type =
                "button";

            restart.textContent =
                "RESTART";

            restart.style.width =
                "100%";

            restart.style.padding =
                "13px 18px";

            restart.style.border =
                "0";

            restart.style.borderRadius =
                "10px";

            restart.style.cursor =
                "pointer";

            restart.style.fontSize =
                "15px";

            restart.style.fontWeight =
                "800";

            restart.style.background =
                "#5ff6ff";

            restart.style.color =
                "#031016";

            restart.onclick =
                function() {

                    if (
                        window.pixelMiner &&
                        typeof window.pixelMiner.restart ===
                        "function"
                    ) {

                        window.pixelMiner.restart();

                    } else {

                        window.location.reload();
                    }
                };

            box.appendChild(title);
            box.appendChild(subtitle);
            box.appendChild(restart);

            popup.appendChild(box);

            document.body.appendChild(
                popup
            );
        }

        popup.style.display =
            "flex";
    }
);

// ============================================================
// HIDE GAME OVER POPUP
// ============================================================

EM_JS(
    void,
    js_hide_game_over,
    (),
    {
        const popup =
            document.getElementById(
                "pixelMinerGameOver"
            );

        if (popup) {
            popup.style.display =
                "none";
        }
    }
);

// ============================================================
// SAVE BEST
// ============================================================

EM_JS(
    void,
    js_save_best,
    (int score),
    {
        try {

            localStorage.setItem(
                "pixel_miner_best",
                String(score)
            );

        } catch (error) {}
    }
);

// ============================================================
// LOAD BEST
// ============================================================

EM_JS(
    int,
    js_load_best,
    (),
    {
        try {

            const value =
                localStorage.getItem(
                    "pixel_miner_best"
                );

            if (!value) {
                return 0;
            }

            const number =
                parseInt(
                    value,
                    10
                );

            if (
                Number.isFinite(
                    number
                )
            ) {
                return number;
            }

        } catch (error) {}

        return 0;
    }
);

// ============================================================
// RENDER
// ============================================================

EM_JS(
    void,
    js_render,
    (
        int mapW,
        int mapH,
        int tile,
        int worldPtr,
        int playerX,
        int playerY,
        int bugCount,
        int bugsPtr,
        int depth
    ),
    {
        if (
            !window.pixelMiner ||
            !window.pixelMiner.ctx ||
            !window.pixelMiner.canvas
        ) {
            return;
        }

        const canvas =
            window.pixelMiner.canvas;

        const ctx =
            window.pixelMiner.ctx;

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

        const maxCameraX =
            Math.max(
                0,
                worldWidth -
                canvas.width
            );

        const maxCameraY =
            Math.max(
                0,
                worldHeight -
                canvas.height
            );

        cameraX =
            Math.max(
                0,
                Math.min(
                    cameraX,
                    maxCameraX
                )
            );

        cameraY =
            Math.max(
                0,
                Math.min(
                    cameraY,
                    maxCameraY
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
                Math.floor(
                    cameraX / tile
                ) - 1
            );

        const startY =
            Math.max(
                0,
                Math.floor(
                    cameraY / tile
                ) - 1
            );

        const endX =
            Math.min(
                mapW - 1,
                Math.ceil(
                    (cameraX +
                        canvas.width) /
                    tile
                ) + 1
            );

        const endY =
            Math.min(
                mapH - 1,
                Math.ceil(
                    (cameraY +
                        canvas.height) /
                    tile
                ) + 1
            );

        // ----------------------------------------------------
        // TILES
        // ----------------------------------------------------

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

                const screenX =
                    x * tile;

                const screenY =
                    y * tile;

                if (!revealed) {

                    ctx.fillStyle =
                        "#020509";

                    ctx.fillRect(
                        screenX,
                        screenY,
                        tile - 1,
                        tile - 1
                    );

                    continue;
                }

                if (type === 0) {

                    ctx.fillStyle =
                        "#1c2d39";

                } else if (type === 1) {

                    ctx.fillStyle =
                        "#3a4852";

                } else if (type === 2) {

                    ctx.fillStyle =
                        "#9b6338";

                } else if (type === 3) {

                    ctx.fillStyle =
                        "#876ce0";

                } else if (type === 4) {

                    ctx.fillStyle =
                        "#277e69";

                } else if (type === 5) {

                    ctx.fillStyle =
                        "#157e67";

                } else if (type === 6) {

                    ctx.fillStyle =
                        "#693b4a";

                } else {

                    ctx.fillStyle =
                        "#18252e";
                }

                ctx.fillRect(
                    screenX,
                    screenY,
                    tile - 1,
                    tile - 1
                );

                if (type === 1) {

                    ctx.fillStyle =
                        "rgba(255,255,255,0.10)";

                    ctx.fillRect(
                        screenX + 4,
                        screenY + 5,
                        6,
                        3
                    );

                    ctx.fillRect(
                        screenX + 14,
                        screenY + 14,
                        5,
                        3
                    );
                }

                if (type === 2) {

                    ctx.fillStyle =
                        "#ffd09b";

                    ctx.fillRect(
                        screenX + 7,
                        screenY + 7,
                        10,
                        10
                    );
                }

                if (type === 3) {

                    ctx.fillStyle =
                        "#eee5ff";

                    ctx.beginPath();

                    ctx.moveTo(
                        screenX + 12,
                        screenY + 4
                    );

                    ctx.lineTo(
                        screenX + 19,
                        screenY + 12
                    );

                    ctx.lineTo(
                        screenX + 12,
                        screenY + 20
                    );

                    ctx.lineTo(
                        screenX + 5,
                        screenY + 12
                    );

                    ctx.closePath();

                    ctx.fill();
                }

                if (type === 4) {

                    ctx.fillStyle =
                        "#c6fff1";

                    ctx.beginPath();

                    ctx.arc(
                        screenX + 12,
                        screenY + 12,
                        6,
                        0,
                        Math.PI * 2
                    );

                    ctx.fill();
                }

                if (type === 5) {

                    ctx.strokeStyle =
                        "#7fffd5";

                    ctx.lineWidth = 2;

                    ctx.beginPath();

                    ctx.arc(
                        screenX + 12,
                        screenY + 12,
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
                        screenX + 12,
                        screenY + 16
                    );
                }

                if (type === 6) {

                    ctx.fillStyle =
                        "#ff7f9d";

                    ctx.beginPath();

                    ctx.arc(
                        screenX + 12,
                        screenY + 12,
                        5,
                        0,
                        Math.PI * 2
                    );

                    ctx.fill();
                }
            }
        }

        // ----------------------------------------------------
        // BUGS
        // ----------------------------------------------------

        if (
            bugCount > 0 &&
            bugsPtr
        ) {

            const bugHeap =
                HEAPF32;

            const bugBase =
                bugsPtr >> 2;

            for (
                let i = 0;
                i < bugCount;
                i++
            ) {

                const bx =
                    bugHeap[
                        bugBase +
                        i * 3
                    ];

                const by =
                    bugHeap[
                        bugBase +
                        i * 3 +
                        1
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

        // ----------------------------------------------------
        // PLAYER
        // ----------------------------------------------------

        const playerScreenX =
            playerX * tile + 12;

        const playerScreenY =
            playerY * tile + 12;

        ctx.fillStyle =
            "#5ff6ff";

        ctx.beginPath();

        ctx.arc(
            playerScreenX,
            playerScreenY,
            9,
            0,
            Math.PI * 2
        );

        ctx.fill();

        ctx.fillStyle =
            "#ffffff";

        ctx.beginPath();

        ctx.arc(
            playerScreenX - 3,
            playerScreenY - 3,
            2,
            0,
            Math.PI * 2
        );

        ctx.arc(
            playerScreenX + 3,
            playerScreenY - 3,
            2,
            0,
            Math.PI * 2
        );

        ctx.fill();

        ctx.restore();

        // ----------------------------------------------------
        // DEPTH HUD
        // ----------------------------------------------------

        ctx.fillStyle =
            "rgba(0,0,0,0.55)";

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
    }
);

// ============================================================
// WORLD GENERATION
// ============================================================

static void generateWorld() {

    world.clear();

    world.resize(
        MAP_W * MAP_H
    );

    const int depth =
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

            Tile tile{};

            tile.revealed = 0;

            const int roll =
                randomInt(
                    0,
                    99
                );

            const int rockChance =
                min(
                    32,
                    22 +
                    depth * 2
                );

            if (
                roll < rockChance
            ) {

                tile.type = 1;

            } else if (
                roll < 78
            ) {

                tile.type = 0;

            } else if (
                roll < 91
            ) {

                tile.type = 2;

            } else if (
                roll < 97
            ) {

                tile.type = 3;

            } else {

                tile.type = 4;
            }

            world[
                indexOf(x, y)
            ] = tile;
        }
    }

    const int startX =
        MAP_W / 2;

    const int startY =
        MAP_H / 2;

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
                startX + dx;

            const int y =
                startY + dy;

            if (
                !insideMap(
                    x,
                    y
                )
            ) {
                continue;
            }

            Tile& tile =
                world[
                    indexOf(
                        x,
                        y
                    )
                ];

            tile.type = 0;
            tile.revealed = 1;
        }
    }

    int exitX = 0;
    int exitY = 0;

    do {

        exitX =
            randomInt(
                3,
                MAP_W - 4
            );

        exitY =
            randomInt(
                3,
                MAP_H - 4
            );

    } while (
        abs(exitX - startX) +
        abs(exitY - startY) <
        12
    );

    world[
        indexOf(
            exitX,
            exitY
        )
    ].type = 5;

    world[
        indexOf(
            exitX,
            exitY
        )
    ].revealed = 1;

    const int hazardCount =
        min(
            10,
            3 + depth
        );

    for (
        int i = 0;
        i < hazardCount;
        i++
    ) {

        const int hazardX =
            randomInt(
                2,
                MAP_W - 3
            );

        const int hazardY =
            randomInt(
                2,
                MAP_H - 3
            );

        if (
            abs(
                hazardX -
                startX
            ) +
            abs(
                hazardY -
                startY
            ) < 7
        ) {
            continue;
        }

        world[
            indexOf(
                hazardX,
                hazardY
            )
        ].type = 6;
    }
}

// ============================================================
// REVEAL
// ============================================================

static void revealAround(
    int centerX,
    int centerY
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
                centerX + dx;

            const int y =
                centerY + dy;

            if (
                insideMap(
                    x,
                    y
                )
            ) {

                world[
                    indexOf(
                        x,
                        y
                    )
                ].revealed = 1;
            }
        }
    }
}

// ============================================================
// MINE / COLLECT
// ============================================================

static void mineTile(
    int x,
    int y
) {

    if (
        !insideMap(x, y)
    ) {
        return;
    }

    Tile& tile =
        world[
            indexOf(
                x,
                y
            )
        ];

    tile.revealed = 1;

    if (
        tile.type == 1
    ) {

        tile.type = 0;

        player.score +=
            1 +
            player.depth;

        player.energy -= 2;

    } else if (
        tile.type == 2
    ) {

        tile.type = 0;

        player.copper++;

        player.score +=
            10 +
            player.depth * 2;

        player.energy -= 2;

    } else if (
        tile.type == 3
    ) {

        tile.type = 0;

        player.crystals++;

        player.score +=
            25 +
            player.depth * 3;

        player.energy -= 2;

    } else if (
        tile.type == 4
    ) {

        tile.type = 0;

        player.score += 15;

        player.energy += 20;

        player.energy =
            min(
                player.energy,
                START_ENERGY
            );

    } else if (
        tile.type == 6
    ) {

        tile.type = 0;

        player.hp -= 12;

        player.score += 3;
    }

    player.energy =
        max(
            0,
            player.energy
        );

    player.hp =
        max(
            0,
            player.hp
        );
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

    // Energy at zero does NOT cause game over.
    // Player simply waits for automatic regeneration.
    if (
        player.energy <= 0
    ) {

        player.energy = 0;

        return;
    }

    const int nextX =
        player.x + dx;

    const int nextY =
        player.y + dy;

    if (
        !insideMap(
            nextX,
            nextY
        )
    ) {
        return;
    }

    Tile& target =
        world[
            indexOf(
                nextX,
                nextY
            )
        ];

    if (
        target.type == 1
    ) {

        mineTile(
            nextX,
            nextY
        );

        revealAround(
            player.x,
            player.y
        );

        if (
            player.hp <= 0
        ) {

            player.hp = 0;

            gameOver = true;

            js_message(
                "GAME OVER"
            );

            js_show_game_over();
        }

        return;
    }

    player.x = nextX;
    player.y = nextY;

    player.energy--;

    revealAround(
        player.x,
        player.y
    );

    if (
        target.type == 2 ||
        target.type == 3 ||
        target.type == 4 ||
        target.type == 6
    ) {

        mineTile(
            nextX,
            nextY
        );
    }

    if (
        target.type == 5
    ) {

        player.depth++;

        player.score +=
            100 *
            player.depth;

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

        const string message =
            "DESCENDED TO DEPTH " +
            to_string(
                player.depth
            );

        js_message(
            message.c_str()
        );
    }

    player.energy =
        max(
            0,
            player.energy
        );

    player.hp =
        max(
            0,
            player.hp
        );

    // HP reaching zero causes Game Over.
    if (
        player.hp <= 0
    ) {

        player.hp = 0;

        gameOver = true;

        js_message(
            "GAME OVER"
        );

        js_show_game_over();
    }
}

// ============================================================
// BUG SPAWN
// ============================================================

static void spawnBug() {

    const int maxBugs =
        min(
            MAX_BUGS,
            2 + player.depth
        );

    if (
        static_cast<int>(
            bugs.size()
        ) >= maxBugs
    ) {
        return;
    }

    Bug bug{};

    bool validPosition =
        false;

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

        const double distance =
            hypot(
                static_cast<double>(
                    bug.x -
                    player.x
                ),
                static_cast<double>(
                    bug.y -
                    player.y
                )
            );

        if (
            distance > 8.0
        ) {

            validPosition =
                true;

            break;
        }
    }

    if (!validPosition) {
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

// ============================================================
// BUG UPDATE
// ============================================================

static void updateBugs(
    double deltaTime
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
            static_cast<double>(
                player.x
            ) -
            bug.x;

        const double dy =
            static_cast<double>(
                player.y
            ) -
            bug.y;

        const double distance =
            sqrt(
                dx * dx +
                dy * dy
            );

        if (
            distance > 0.05
        ) {

            bug.x +=
                static_cast<float>(
                    dx /
                    distance *
                    bug.speed *
                    deltaTime
                );

            bug.y +=
                static_cast<float>(
                    dy /
                    distance *
                    bug.speed *
                    deltaTime
                );
        }

        if (
            distance < 0.7
        ) {

            player.hp -=
                8 +
                player.depth;

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

            player.hp =
                max(
                    0,
                    player.hp
                );

            if (
                player.hp <= 0
            ) {

                player.hp = 0;

                gameOver = true;

                js_message(
                    "GAME OVER"
                );

                js_show_game_over();

                return;
            }
        }
    }
}

// ============================================================
// ENERGY REGENERATION
// ============================================================

static void updateEnergy(
    double currentTime
) {

    if (
        gameOver ||
        paused
    ) {
        return;
    }

    if (
        player.energy >= START_ENERGY
    ) {

        player.energy =
            START_ENERGY;

        lastEnergyRegen =
            currentTime;

        return;
    }

    if (
        currentTime -
        lastEnergyRegen >=
        1.0
    ) {

        const double elapsed =
            currentTime -
            lastEnergyRegen;

        const int seconds =
            static_cast<int>(
                elapsed
            );

        if (
            seconds > 0
        ) {

            player.energy =
                min(
                    START_ENERGY,
                    player.energy +
                    seconds *
                    ENERGY_REGEN_PER_SECOND
                );

            lastEnergyRegen +=
                static_cast<double>(
                    seconds
                );
        }
    }
}

// ============================================================
// KEYBOARD
// ============================================================

static void handleKeyboard() {

    if (
        js_key(0)
    ) {

        tryMove(
            0,
            -1
        );

    } else if (
        js_key(1)
    ) {

        tryMove(
            0,
            1
        );

    } else if (
        js_key(2)
    ) {

        tryMove(
            -1,
            0
        );

    } else if (
        js_key(3)
    ) {

        tryMove(
            1,
            0
        );
    }
}

// ============================================================
// MOUSE
// ============================================================

static void handleMouse() {

    if (
        !js_mouse_down()
    ) {
        return;
    }

    const int mouseX =
        js_mouse_x();

    const int mouseY =
        js_mouse_y();

    // Mouse coordinates are canvas/screen coordinates.
    // Convert them to the nearest adjacent direction
    // around the player rather than treating them as world
    // coordinates.

    const int canvasCenterX =
        MAP_W * TILE / 2;

    const int canvasCenterY =
        MAP_H * TILE / 2;

    const int relativeX =
        mouseX - canvasCenterX;

    const int relativeY =
        mouseY - canvasCenterY;

    if (
        abs(relativeX) >
        abs(relativeY)
    ) {

        if (
            relativeX > 0
        ) {

            tryMove(
                1,
                0
            );

        } else {

            tryMove(
                -1,
                0
            );
        }

    } else {

        if (
            relativeY > 0
        ) {

            tryMove(
                0,
                1
            );

        } else {

            tryMove(
                0,
                -1
            );
        }
    }
}

// ============================================================
// RENDER
// ============================================================

static void renderGame() {

    if (
        world.empty()
    ) {
        return;
    }

    const int worldPointer =
        static_cast<int>(
            reinterpret_cast<uintptr_t>(
                world.data()
            )
        );

    const int bugPointer =
        bugs.empty()
            ? 0
            : static_cast<int>(
                reinterpret_cast<uintptr_t>(
                    bugs.data()
                )
            );

    js_render(
        MAP_W,
        MAP_H,
        TILE,
        worldPointer,
        player.x,
        player.y,
        static_cast<int>(
            bugs.size()
        ),
        bugPointer,
        player.depth
    );
}

// ============================================================
// HUD
// ============================================================

static void updateHUD() {

    js_update_hud(
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
// EXPORTED MOVE
// ============================================================

extern "C"
EMSCRIPTEN_KEEPALIVE
void move_player(
    int dx,
    int dy
) {

    if (
        gameOver ||
        paused
    ) {
        return;
    }

    tryMove(
        dx,
        dy
    );
}

// ============================================================
// EXPORTED PAUSE
// ============================================================

extern "C"
EMSCRIPTEN_KEEPALIVE
void toggle_pause() {

    if (gameOver) {
        return;
    }

    paused =
        !paused;

    if (paused) {

        js_message(
            "PAUSED"
        );

    } else {

        js_message(
            "MINING"
        );
    }
}

// ============================================================
// EXPORTED RESTART
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

    lastMoveTime = 0.0;
    lastBugSpawn = 0.0;

    lastEnergyRegen =
        emscripten_get_now() /
        1000.0;

    generateWorld();

    revealAround(
        player.x,
        player.y
    );

    js_hide_game_over();

    js_message(
        "MINING"
    );

    updateHUD();

    renderGame();
}

// ============================================================
// GAME LOOP
// ============================================================

static void gameLoop(
    void*
) {

    static double previousTime =
        0.0;

    const double currentTime =
        emscripten_get_now() /
        1000.0;

    double deltaTime =
        currentTime -
        previousTime;

    previousTime =
        currentTime;

    if (
        deltaTime < 0.0
    ) {
        deltaTime = 0.0;
    }

    if (
        deltaTime > 0.1
    ) {
        deltaTime = 0.1;
    }

    if (
        !gameOver &&
        !paused
    ) {

        // --------------------------------------------
        // ENERGY REGEN
        // --------------------------------------------

        updateEnergy(
            currentTime
        );

        // --------------------------------------------
        // PLAYER INPUT
        // --------------------------------------------

        if (
            currentTime -
            lastMoveTime >=
            MOVE_COOLDOWN
        ) {

            handleKeyboard();

            handleMouse();

            lastMoveTime =
                currentTime;
        }

        // --------------------------------------------
        // BUG SPAWN
        // --------------------------------------------

        const double spawnInterval =
            max(
                2.5,
                6.0 -
                player.depth * 0.2
            );

        if (
            currentTime -
            lastBugSpawn >=
            spawnInterval
        ) {

            spawnBug();

            lastBugSpawn =
                currentTime;
        }

        // --------------------------------------------
        // BUG UPDATE
        // --------------------------------------------

        updateBugs(
            deltaTime
        );

        // --------------------------------------------
        // BEST SCORE
        // --------------------------------------------

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

    renderGame();
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

    lastEnergyRegen =
        emscripten_get_now() /
        1000.0;

    generateWorld();

    revealAround(
        player.x,
        player.y
    );

    js_hide_game_over();

    js_message(
        "MINING"
    );

    updateHUD();

    renderGame();

    emscripten_set_main_loop_arg(
        gameLoop,
        nullptr,
        0,
        1
    );

    return 0;
}
