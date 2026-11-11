/* The starters' error messages (tlou-launcher.c, tlou-game.c) in the launcher's languages. The
 * language is the one picked in the launcher (ui_language in analysis\tlou-user\launcher.json),
 * else Windows' display language, else English, as the launcher's own default. */
#ifndef TLOU_MESSAGES_H
#define TLOU_MESSAGES_H

#include <windows.h>
#include <string.h>
#include <wchar.h>

enum { MSG_NO_LAUNCHER_SCRIPT, MSG_LAUNCHER_FAILED, MSG_NO_GAME_SCRIPT, MSG_GAME_FAILED, MSG_COUNT };

/* Same order as the launcher's UI_LANGUAGES. */
static const char *const LANGUAGE_CODES[] = {"pt", "en", "es", "fr", "de", "it", "pl", "ru", "ja"};
#define LANGUAGE_COUNT (sizeof(LANGUAGE_CODES) / sizeof(LANGUAGE_CODES[0]))
#define ENGLISH 1

static const wchar_t *const MESSAGES[LANGUAGE_COUNT][MSG_COUNT] = {
    {   /* pt */
        L"tlou_launcher.pyw não foi encontrado ao lado deste arquivo.",
        L"O launcher não abriu. Confira se o Python está instalado e no PATH.",
        L"tools\\run_game.py não foi encontrado ao lado deste arquivo.",
        L"O jogo não abriu. Confira se o Python está instalado e no PATH; "
        L"o log é analysis\\tlou\\launcher-run.log.",
    },
    {   /* en */
        L"tlou_launcher.pyw was not found beside this file.",
        L"The launcher did not open. Check that Python is installed and on the PATH.",
        L"tools\\run_game.py was not found beside this file.",
        L"The game did not open. Check that Python is installed and on the PATH; "
        L"the log is analysis\\tlou\\launcher-run.log.",
    },
    {   /* es */
        L"No se encontró tlou_launcher.pyw junto a este archivo.",
        L"El launcher no se abrió. Comprueba que Python esté instalado y en el PATH.",
        L"No se encontró tools\\run_game.py junto a este archivo.",
        L"El juego no se abrió. Comprueba que Python esté instalado y en el PATH; "
        L"el registro está en analysis\\tlou\\launcher-run.log.",
    },
    {   /* fr */
        L"tlou_launcher.pyw est introuvable à côté de ce fichier.",
        L"Le launcher ne s'est pas ouvert. Vérifiez que Python est installé et dans le PATH.",
        L"tools\\run_game.py est introuvable à côté de ce fichier.",
        L"Le jeu ne s'est pas ouvert. Vérifiez que Python est installé et dans le PATH ; "
        L"le journal est analysis\\tlou\\launcher-run.log.",
    },
    {   /* de */
        L"tlou_launcher.pyw wurde neben dieser Datei nicht gefunden.",
        L"Der Launcher wurde nicht geöffnet. Prüfe, ob Python installiert und im PATH ist.",
        L"tools\\run_game.py wurde neben dieser Datei nicht gefunden.",
        L"Das Spiel wurde nicht gestartet. Prüfe, ob Python installiert und im PATH ist; "
        L"das Protokoll ist analysis\\tlou\\launcher-run.log.",
    },
    {   /* it */
        L"tlou_launcher.pyw non è stato trovato accanto a questo file.",
        L"Il launcher non si è aperto. Controlla che Python sia installato e nel PATH.",
        L"tools\\run_game.py non è stato trovato accanto a questo file.",
        L"Il gioco non si è aperto. Controlla che Python sia installato e nel PATH; "
        L"il log è analysis\\tlou\\launcher-run.log.",
    },
    {   /* pl */
        L"Nie znaleziono tlou_launcher.pyw obok tego pliku.",
        L"Launcher się nie otworzył. Sprawdź, czy Python jest zainstalowany i w zmiennej PATH.",
        L"Nie znaleziono tools\\run_game.py obok tego pliku.",
        L"Gra się nie uruchomiła. Sprawdź, czy Python jest zainstalowany i w zmiennej PATH; "
        L"log znajduje się w analysis\\tlou\\launcher-run.log.",
    },
    {   /* ru */
        L"Файл tlou_launcher.pyw не найден "
        L"рядом с этим файлом.",
        L"Лаунчер не открылся. "
        L"Проверьте, что Python "
        L"установлен и добавлен "
        L"в PATH.",
        L"Файл tools\\run_game.py не найден "
        L"рядом с этим файлом.",
        L"Игра не запустилась. "
        L"Проверьте, что Python "
        L"установлен и добавлен "
        L"в PATH; журнал: analysis\\tlou\\launcher-run.log.",
    },
    {   /* ja */
        L"このファイルの隣に tlou_launcher.pyw "
        L"が見つかりません。",
        L"ランチャーを開けませんでした。"
        L"Python がインストールされ、PATH "
        L"に含まれているか確認してください。",
        L"このファイルの隣に tools\\run_game.py "
        L"が見つかりません。",
        L"ゲームを開始できませんでした。"
        L"Python がインストールされ、PATH "
        L"に含まれているか確認してください。"
        L"ログ: analysis\\tlou\\launcher-run.log",
    },
};

/* ui_language from <root>\analysis\tlou-user\launcher.json; -1 when missing or unknown. */
static int saved_language(const wchar_t *root)
{
    static wchar_t path[32768];
    static char text[65536];
    swprintf(path, 32768, L"%ls\\analysis\\tlou-user\\launcher.json", root);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    DWORD read = 0;
    BOOL ok = ReadFile(file, text, sizeof(text) - 1, &read, NULL);
    CloseHandle(file);
    if (!ok)
        return -1;
    text[read] = 0;
    const char *key = strstr(text, "\"ui_language\"");
    if (!key)
        return -1;
    const char *value = strchr(key + 13, '"');
    if (!value)
        return -1;
    value++;
    for (size_t i = 0; i < LANGUAGE_COUNT; i++)
        if (strncmp(value, LANGUAGE_CODES[i], 2) == 0 && value[2] == '"')
            return (int)i;
    return -1;
}

static int windows_language(void)
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_PORTUGUESE: return 0;
    case LANG_SPANISH: return 2;
    case LANG_FRENCH: return 3;
    case LANG_GERMAN: return 4;
    case LANG_ITALIAN: return 5;
    case LANG_POLISH: return 6;
    case LANG_RUSSIAN: return 7;
    case LANG_JAPANESE: return 8;
    default: return ENGLISH;
    }
}

/* dir: the starter's folder. The settings are under it (release package, TLOU Game.exe in the
 * repository) or under its parent (TLOU Launcher.exe in the repository's launcher\). */
static const wchar_t *message(const wchar_t *dir, int id)
{
    static wchar_t parent[32768];
    int language = saved_language(dir);
    if (language < 0) {
        swprintf(parent, 32768, L"%ls\\..", dir);
        language = saved_language(parent);
    }
    if (language < 0)
        language = windows_language();
    return MESSAGES[language][id];
}

#endif
