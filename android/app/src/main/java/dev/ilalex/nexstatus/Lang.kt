package dev.ilalex.nexstatus

import java.util.Locale

enum class LangChoice { System, Russian, English }

enum class MainTab { Games, Account }

data class Copy(
    val online: String,
    val empty: String,
    val searchingServers: String,
    val gamesWithPlayers: (Int, String) -> String,
    val playersWord: (Int) -> String,
    val gamesWord: (Int) -> String,
    val lobbies: (Int) -> String,
    val players: (Int) -> String,
    val signIn: String,
    val signOut: String,
    val waitingBrowser: String,
    val signInToSee: String,
    val aboutGame: String,
    val aboutApp: String,
    val application: String,
    val version: String,
    val author: String,
    val language: String,
    val system: String,
    val russian: String,
    val english: String,
    val gamesTab: String,
    val accountTab: String,
    val noDescription: String,
    val loadingGameInfo: String,
    val gameInfoFailed: String,
    val account: String,
    val updateTitle: String,
    val updateBody: (String) -> String,
    val updateNow: String,
    val updateLater: String,
    val updateDownloading: String,
    val updateFailed: String,
    val updateRetry: String,
)

fun resolveLang(choice: LangChoice): Boolean {
    // true = Russian
    return when (choice) {
        LangChoice.Russian -> true
        LangChoice.English -> false
        LangChoice.System -> Locale.getDefault().language.startsWith("ru")
    }
}

fun copyFor(russian: Boolean): Copy {
    if (russian) {
        return Copy(
            online = "онлайн",
            empty = "Пусто",
            searchingServers = "Ищем серверы",
            gamesWithPlayers = { n, word -> "$n $word с игроками" },
            playersWord = ::playersWordRu,
            gamesWord = ::gamesWordRu,
            lobbies = { n -> "Лобби ($n)" },
            players = { n -> "Игроки ($n)" },
            signIn = "Войти",
            signOut = "Выйти",
            waitingBrowser = "Ждём браузер…",
            signInToSee = "Войдите, чтобы увидеть лобби и игроков.",
            aboutGame = "Об игре",
            aboutApp = "О приложении",
            application = "Приложение",
            version = "Версия",
            author = "Автор",
            language = "Язык",
            system = "Система",
            russian = "Русский",
            english = "English",
            gamesTab = "Игры",
            accountTab = "Аккаунт",
            noDescription = "Описание недоступно.",
            loadingGameInfo = "Загружаем описание…",
            gameInfoFailed = "Не удалось загрузить описание.",
            account = "Аккаунт",
            updateTitle = "Доступно обновление",
            updateBody = { "NexStatus $it" },
            updateNow = "Обновить",
            updateLater = "Позже",
            updateDownloading = "Загрузка…",
            updateFailed = "Не удалось скачать обновление.",
            updateRetry = "Повторить",
        )
    }
    return Copy(
        online = "online",
        empty = "Empty",
        searchingServers = "Looking for servers",
        gamesWithPlayers = { n, word -> "$n $word with players" },
        playersWord = { n -> if (n == 1) "player" else "players" },
        gamesWord = { n -> if (n == 1) "game" else "games" },
        lobbies = { n -> "Lobbies ($n)" },
        players = { n -> "Players ($n)" },
        signIn = "Sign in",
        signOut = "Sign out",
        waitingBrowser = "Waiting for browser…",
        signInToSee = "Sign in to see lobbies and players.",
        aboutGame = "About game",
        aboutApp = "About",
        application = "Application",
        version = "Version",
        author = "Author",
        language = "Language",
        system = "System",
        russian = "Русский",
        english = "English",
        gamesTab = "Games",
        accountTab = "Account",
        noDescription = "Description unavailable.",
        loadingGameInfo = "Loading description…",
        gameInfoFailed = "Couldn't load description.",
        account = "Account",
        updateTitle = "Update available",
        updateBody = { "NexStatus $it" },
        updateNow = "Update",
        updateLater = "Later",
        updateDownloading = "Downloading…",
        updateFailed = "Couldn't download the update.",
        updateRetry = "Retry",
    )
}

private fun playersWordRu(count: Int) = when {
    count % 10 == 1 && count % 100 != 11 -> "игрок"
    count % 10 in 2..4 && count % 100 !in 12..14 -> "игрока"
    else -> "игроков"
}

private fun gamesWordRu(count: Int) = when {
    count % 10 == 1 && count % 100 != 11 -> "игра"
    count % 10 in 2..4 && count % 100 !in 12..14 -> "игры"
    else -> "игр"
}
