// main.cpp
// Cross-platform Terminal Racer (Linux/macOS + Windows)
// Option 1: single codebase with #ifdef _WIN32 branches.

#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cctype>

// Platform-specific headers
#ifdef _WIN32
    #include <conio.h>      // _kbhit, _getch
    #include <windows.h>    // Console APIs
#else
    #include <unistd.h>
    #include <termios.h>
    #include <sys/ioctl.h>
    #include <sys/select.h>
#endif

// --- Configuration ---
const int TRACK_WIDTH = 20;
const int SCREEN_HEIGHT = 20;
const int START_PLAYER_X = TRACK_WIDTH / 2 + 1;
const std::string HIGHSCORE_FILE = "highscore.txt";

const char PLAYER_CHAR = '@';
const char OBSTACLE_CHAR = '#';
const char ROAD_CHAR = ' ';
const char BORDER_CHAR = '|';

// ANSI Clear screen (works on most modern terminals, and on Windows 10+ when VT is enabled)
const std::string CLEAR_SCREEN = "\033[2J\033[1;1H";

// --- Global Game State ---
struct Car { int x; int y; };

std::vector<Car> obstacles;
int playerX = START_PLAYER_X;
long long score = 0;
bool gameOver = false;

// --- Game Settings & Persistence ---
long long highestScore = 0;
int difficultyLevel = 1; // 1..5

// Controls (store sequences uniformly across platforms)
std::string moveLeftKey = "a";
std::string moveRightKey = "d";

#ifdef _WIN32
    // Windows console saved state
    static DWORD originalConsoleMode = 0;
    static HANDLE hStdin = nullptr;
    static HANDLE hStdout = nullptr;
#else
    // Unix saved state
    struct termios originalTermios;
#endif

// --- Terminal utilities (cross-platform) ---

void gotoxy(int y, int x) {
#ifdef _WIN32
    if (!hStdout) hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD pos;
    pos.X = (SHORT)(x - 1);
    pos.Y = (SHORT)(y - 1);
    SetConsoleCursorPosition(hStdout, pos);
#else
    std::cout << "\033[" << y << ";" << x << "H";
#endif
}

void hideCursor() {
#ifdef _WIN32
    if (!hStdout) hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO info;
    GetConsoleCursorInfo(hStdout, &info);
    info.bVisible = FALSE;
    SetConsoleCursorInfo(hStdout, &info);
#else
    std::cout << "\033[?25l";
#endif
}

void showCursor() {
#ifdef _WIN32
    if (!hStdout) hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO info;
    GetConsoleCursorInfo(hStdout, &info);
    info.bVisible = TRUE;
    SetConsoleCursorInfo(hStdout, &info);
#else
    std::cout << "\033[?25h";
#endif
}

void setupTerminal() {
#ifdef _WIN32
    // Get handles
    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    // Save original console mode
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    originalConsoleMode = mode;
    // Disable line input and echo input so _getch/_kbhit behave as expected
    // Keep ENABLE_WINDOW_INPUT and ENABLE_MOUSE_INPUT off by default
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    SetConsoleMode(hStdin, mode);

    // Enable Virtual Terminal Processing on output so ANSI codes work (Windows 10+)
    DWORD outMode = 0;
    GetConsoleMode(hStdout, &outMode);
    outMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hStdout, outMode);

#else
    struct termios newt;
    tcgetattr(STDIN_FILENO, &originalTermios);
    newt = originalTermios;
    newt.c_lflag &= ~(ICANON | ECHO); // non-canonical, no-echo
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
#endif
}

void restoreTerminal() {
#ifdef _WIN32
    if (originalConsoleMode) {
        SetConsoleMode(hStdin, originalConsoleMode);
    }
    showCursor();
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios);
    std::cout << "\033[?25h";
#endif
    std::cout.flush();
}

/**
 * Reads an input key or sequence without blocking.
 * Returns:
 *  - empty string if nothing was pressed
 *  - single printable char string for printable keys
 *  - escape-like sequences for arrows: "\033[A" etc (consistent across platforms)
 */
std::string getInputSequence() {
#ifdef _WIN32
    if (!_kbhit()) return std::string();
    int ch = _getch();
    if (ch == 0 || ch == 224) {
        // special key: read next code
        int code = _getch();
        switch (code) {
            case 72: return "\033[A"; // Up
            case 80: return "\033[B"; // Down
            case 77: return "\033[C"; // Right
            case 75: return "\033[D"; // Left
            default:
                // Return a descriptive sequence with code
                {
                    std::ostringstream oss;
                    oss << "WIN_SEQ(" << code << ")";
                    return oss.str();
                }
        }
    } else {
        // Normal character
        char c = static_cast<char>(ch);
        return std::string(1, c);
    }
#else
    std::string res;
    char c = 0;

    fd_set rdset;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    FD_ZERO(&rdset);
    FD_SET(STDIN_FILENO, &rdset);

    if (select(STDIN_FILENO + 1, &rdset, NULL, NULL, &tv) > 0) {
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r > 0) {
            res.push_back(c);
            if (c == '\033') { // potential escape sequence
                const int maxWaitMs = 100;
                int waited = 0;
                while (waited < maxWaitMs) {
                    int bytesAvailable = 0;
                    ioctl(STDIN_FILENO, FIONREAD, &bytesAvailable);
                    if (bytesAvailable > 0) {
                        char buf[10];
                        ssize_t r2 = read(STDIN_FILENO, buf, std::min(bytesAvailable, 9));
                        if (r2 > 0) res.append(buf, buf + r2);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    waited += 8;
                }
            }
        }
    }
    return res;
#endif
}

std::string keyToDisplay(const std::string &k) {
    if (k.empty()) return "NONE";
    if (k == "\033[A") return "UP_ARROW";
    if (k == "\033[B") return "DOWN_ARROW";
    if (k == "\033[C") return "RIGHT_ARROW";
    if (k == "\033[D") return "LEFT_ARROW";
    if (k == "\n" || k == "\r") return "ENTER";
    if (k == " ") return "SPACE";
    if (k == "\t") return "TAB";
    if (k.size() == 1 && isprint(static_cast<unsigned char>(k[0]))) {
        return std::string(1, k[0]);
    }
    std::ostringstream oss;
    oss << "SEQ(";
    for (unsigned char ch : k) {
        oss << "0x" << std::hex << std::uppercase << (int)ch << " ";
    }
    oss << ")";
    return oss.str();
}

// --- High Score Persistence ---
void loadHighestScore() {
    std::ifstream file(HIGHSCORE_FILE);
    if (file.is_open()) {
        file >> highestScore;
        file.close();
    }
}
void saveHighestScore() {
    if (score > highestScore) {
        highestScore = score;
        std::ofstream file(HIGHSCORE_FILE);
        if (file.is_open()) {
            file << highestScore;
            file.close();
        }
    }
}

// --- Game Logic ---
void updateObstacles() {
    for (auto &obs : obstacles) obs.y++;
    if (!obstacles.empty() && obstacles.front().y > SCREEN_HEIGHT) {
        obstacles.erase(obstacles.begin());
        score += 10;
    }
    bool shouldSpawn = (rand() % 10 < 3 && obstacles.empty()) ||
                       (!obstacles.empty() && obstacles.back().y > 2);
    if (shouldSpawn) {
        int newX = (rand() % TRACK_WIDTH) + 2;
        obstacles.push_back({newX, 1});
    }
}
void checkCollision() {
    for (const auto &obs : obstacles) {
        if (obs.y == SCREEN_HEIGHT && obs.x == playerX) {
            gameOver = true;
            break;
        }
    }
}

// --- Rendering ---
void draw() {
    gotoxy(1,1);
    for (int y = 1; y <= SCREEN_HEIGHT; ++y) {
        for (int x = 1; x <= TRACK_WIDTH + 2; ++x) {
            if (x == 1 || x == TRACK_WIDTH + 2) {
                std::cout << BORDER_CHAR;
            } else if (y == SCREEN_HEIGHT && x == playerX) {
                std::cout << PLAYER_CHAR;
            } else {
                bool isObstacle = false;
                for (const auto &obs : obstacles) {
                    if (obs.y == y && obs.x == x) {
                        std::cout << OBSTACLE_CHAR;
                        isObstacle = true;
                        break;
                    }
                }
                if (!isObstacle) std::cout << ROAD_CHAR;
            }
        }
        std::cout << "\n";
    }
    gotoxy(SCREEN_HEIGHT + 1, 1);
    std::cout << "Score: " << score << " | Level: " << difficultyLevel
              << " | Controls: Left=" << keyToDisplay(moveLeftKey)
              << " Right=" << keyToDisplay(moveRightKey) << "    " << std::flush;
}

// --- Menus ---
void showControlsMenu() {
    std::string newKey;
    std::cout << CLEAR_SCREEN;
    gotoxy(2,1);
    std::cout << "--- CONTROL CUSTOMIZATION ---\n\n";
    std::cout << "Current Left Key : " << keyToDisplay(moveLeftKey) << "\n";
    std::cout << "Current Right Key: " << keyToDisplay(moveRightKey) << "\n\n";
    std::cout << "Press any key now to set NEW Left control (arrow keys work)." << std::flush;

    newKey.clear();
    while (newKey.empty()) {
        newKey = getInputSequence();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    moveLeftKey = newKey;

    std::cout << "\n\nLeft key assigned to: " << keyToDisplay(moveLeftKey)
              << "\nNow press any key to set NEW Right control (arrow keys work)." << std::flush;

    newKey.clear();
    while (newKey.empty()) {
        newKey = getInputSequence();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    moveRightKey = newKey;

    std::cout << "\n\nRight key assigned to: " << keyToDisplay(moveRightKey) << "\n\n";
    std::cout << "Controls Updated! Left: '" << keyToDisplay(moveLeftKey)
              << "'  Right: '" << keyToDisplay(moveRightKey) << "'\n\n";
    std::cout << "Press ENTER to return to the menu..." << std::flush;

    // Wait for ENTER in canonical mode
    restoreTerminal();
    std::string tmp;
    std::getline(std::cin, tmp);
    setupTerminal();
}

void showLevelSelect() {
    int inputLevel = difficultyLevel;
    std::cout << CLEAR_SCREEN;
    gotoxy(SCREEN_HEIGHT/2 - 2, 1);
    std::cout << "--- SELECT DIFFICULTY ---";
    gotoxy(SCREEN_HEIGHT/2, 1);
    std::cout << "Levels: 1 (Easy) to 5 (Hardest). Current: " << difficultyLevel;
    gotoxy(SCREEN_HEIGHT/2 + 1, 1);
    std::cout << "Enter new level (1-5) and press ENTER: ";
    restoreTerminal();
    if (!(std::cin >> inputLevel)) {
        inputLevel = difficultyLevel;
        std::cin.clear();
    }
    std::cin.ignore(1000, '\n');
    if (inputLevel >= 1 && inputLevel <= 5) difficultyLevel = inputLevel;
    std::cout << "Level set to " << difficultyLevel << ". Press ENTER to return to menu.";
    std::cin.get();
    setupTerminal();
}

int showMenu() {
    int choice = 0;
    while (true) {
        std::cout << CLEAR_SCREEN;
        gotoxy(2,1);
        std::cout << "--- TERMINAL RACER MENU ---";
        gotoxy(4,1);
        std::cout << "1. New Game (Level: " << difficultyLevel << ")";
        gotoxy(5,1);
        std::cout << "2. Select Level (1-5)";
        gotoxy(6,1);
        std::cout << "3. Controls (Left: '" << keyToDisplay(moveLeftKey) << "', Right: '" << keyToDisplay(moveRightKey) << "')";
        gotoxy(7,1);
        std::cout << "4. Highest Score: " << highestScore;
        gotoxy(8,1);
        std::cout << "5. Exit";
        gotoxy(10,1);
        std::cout << "Enter choice (1-5) and press ENTER: ";
        restoreTerminal();
        if (!(std::cin >> choice)) { choice = 0; std::cin.clear(); }
        std::cin.ignore(1000,'\n');
        setupTerminal();

        switch (choice) {
            case 1: return 1;
            case 2: showLevelSelect(); break;
            case 3: showControlsMenu(); break;
            case 4: {
                gotoxy(12,1);
                std::cout << "Highest score displayed. Press ENTER to return to menu.";
                restoreTerminal(); { std::string tmp; std::getline(std::cin, tmp); } setupTerminal();
                break;
            }
            case 5: return 5;
            default:
                gotoxy(12,1);
                std::cout << "Invalid choice. Press ENTER to continue...";
                restoreTerminal(); { std::string tmp; std::getline(std::cin, tmp); } setupTerminal();
                break;
        }
    }
}

// --- Main Game ---
void gameLoop() {
    gameOver = false;
    score = 0;
    playerX = START_PLAYER_X;
    obstacles.clear();

    std::cout << CLEAR_SCREEN;
    hideCursor();

    int msDuration = 120 - (difficultyLevel * 20);
    if (msDuration < 20) msDuration = 20;
    const std::chrono::milliseconds updateDuration(msDuration);

    auto lastUpdateTime = std::chrono::steady_clock::now();

    while (!gameOver) {
        std::string input = getInputSequence();
        if (!input.empty()) {
            if (input == moveLeftKey) {
                if (playerX > 2) playerX--;
            } else if (input == moveRightKey) {
                if (playerX < TRACK_WIDTH + 1) playerX++;
            } else if (input == "q" || input == "Q" || input == "\x03") {
                gameOver = true;
            }
        }

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastUpdateTime);
        if (elapsed >= updateDuration) {
            updateObstacles();
            checkCollision();
            lastUpdateTime = currentTime;
        }

        draw();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    saveHighestScore();
}

// --- main ---
int main() {
    std::srand((unsigned)std::time(nullptr));
    loadHighestScore();

    setupTerminal();

    int menuChoice = 0;
    try {
        do {
            menuChoice = showMenu();
            if (menuChoice == 1) {
                gameLoop();
                restoreTerminal();
                std::cout << "\n\n  *** GAME OVER ***\n";
                std::cout << "  Final Score: " << score << "\n";
                std::cout << "  Highest Score: " << highestScore << "\n\n";
                std::cout << "Press ENTER to return to the main menu...";
                std::cin.get();
                setupTerminal();
            }
        } while (menuChoice != 5);
    } catch (...) {
        restoreTerminal();
        std::cerr << "\n\nAn unexpected error occurred.\n";
        return 1;
    }

    restoreTerminal();
    std::cout << "\n\nThanks for playing Terminal Racer!\n";
    return 0;
}
