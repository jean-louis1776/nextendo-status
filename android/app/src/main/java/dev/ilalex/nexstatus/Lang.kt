package dev.ilalex.nexstatus

import java.util.Locale

enum class LangChoice { System, Russian, English }

enum class MainTab { Games, Account }

data class Copy(
    val online: String,
    val empty: String,
    val offline: String,
    val noStatus: String,
    val serversTitle: String,
    val offlineShort: String,
    val searchingServers: String,
    val gamesWithPlayers: (Int, String) -> String,
    val playersWord: (Int) -> String,
    val gamesWord: (Int) -> String,
    val lobbies: (Int) -> String,
    val players: (Int) -> String,
    val lobbyTitle: (Int) -> String,
    val lobbyLabel: (Int) -> String,
    val lobbyState: (String) -> String,
    val markYou: String,
    val markFriend: String,
    val playersOnlineNote: (Int) -> String,
    val listUnavailable: String,
    val loadLobbiesFailed: String,
    val nobodyHere: String,
    val roomsUnavailable: String,
    val onlineNotInRoom: String,
    val loadListFailed: String,
    val signIn: String,
    val signOut: String,
    val waitingBrowser: String,
    val signInFailed: String,
    val signInOpenFailed: String,
    val signInTimedOut: String,
    val profilePartial: String,
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
    val close: String,
    val updateTitle: String,
    val updateBody: (String) -> String,
    val updateNow: String,
    val updateLater: String,
    val updateDownloading: String,
    val updateFailed: String,
    val updateRetry: String,
    val myGameHistory: String,
    val allMyGames: String,
    val gameHistory: String,
    val historyEmpty: String,
    val historyFailed: String,
    val loadingHistory: String,
    val playHours: (Long) -> String,
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
            offline = "Не в сети",
            noStatus = "Нет статуса",
            serversTitle = "Серверов",
            offlineShort = "Оффлайн",
            searchingServers = "Ищем серверы",
            gamesWithPlayers = { n, word -> "$n $word с игроками" },
            playersWord = ::playersWordRu,
            gamesWord = ::gamesWordRu,
            lobbies = { n -> "Лобби ($n)" },
            players = { n -> "Игроки ($n)" },
            lobbyTitle = { n -> "Лобби $n" },
            lobbyLabel = { n -> "Лобби $n" },
            lobbyState = ::lobbyStateRu,
            markYou = "Вы",
            markFriend = "Друг",
            playersOnlineNote = { n -> "$n ${playersWordRu(n)} онлайн." },
            listUnavailable = "Список игроков и лобби временно недоступен.",
            loadLobbiesFailed = "Не удалось загрузить лобби.",
            nobodyHere = "Сейчас никого нет.",
            roomsUnavailable = "Игроки онлайн. Комнаты временно недоступны.",
            onlineNotInRoom = "Эти игроки онлайн, но ещё не в комнате.",
            loadListFailed = "Не удалось загрузить список.",
            signIn = "Войти",
            signOut = "Выйти",
            waitingBrowser = "Ждём браузер…",
            signInFailed = "Не удалось войти",
            signInOpenFailed = "Не удалось открыть вход",
            signInTimedOut = "Время входа истекло",
            profilePartial = "Вход есть, профиль не загрузился.",
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
            close = "Закрыть",
            updateTitle = "Доступно обновление",
            updateBody = { "NexStatus $it" },
            updateNow = "Обновить",
            updateLater = "Позже",
            updateDownloading = "Загрузка…",
            updateFailed = "Не удалось скачать обновление.",
            updateRetry = "Повторить",
            myGameHistory = "Игровая активность",
            allMyGames = "Вся активность",
            gameHistory = "Игровая активность",
            historyEmpty = "Активность пока пуста.",
            historyFailed = "Не удалось загрузить активность.",
            loadingHistory = "Загружаем активность…",
            playHours = ::playHoursRu,
        )
    }
    return Copy(
        online = "online",
        empty = "Empty",
        offline = "Offline",
        noStatus = "No status",
        serversTitle = "Servers",
        offlineShort = "Offline",
        searchingServers = "Looking for servers",
        gamesWithPlayers = { n, word -> "$n $word with players" },
        playersWord = { n -> if (n == 1) "player" else "players" },
        gamesWord = { n -> if (n == 1) "game" else "games" },
        lobbies = { n -> "Lobbies ($n)" },
        players = { n -> "Players ($n)" },
        lobbyTitle = { n -> "Lobby $n" },
        lobbyLabel = { n -> "Lobby $n" },
        lobbyState = ::lobbyStateEn,
        markYou = "You",
        markFriend = "Friend",
        playersOnlineNote = { n -> "$n ${if (n == 1) "player" else "players"} online." },
        listUnavailable = "Player list and lobbies are temporarily unavailable.",
        loadLobbiesFailed = "Couldn't load lobbies.",
        nobodyHere = "Nobody here right now.",
        roomsUnavailable = "Players are online. Rooms are temporarily unavailable.",
        onlineNotInRoom = "These players are online, but not in a room yet.",
        loadListFailed = "Couldn't load the list.",
        signIn = "Sign in",
        signOut = "Sign out",
        waitingBrowser = "Waiting for browser…",
        signInFailed = "Couldn't sign in",
        signInOpenFailed = "Couldn't open sign-in",
        signInTimedOut = "Sign-in timed out",
        profilePartial = "Signed in, but the profile didn't load.",
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
        close = "Close",
        updateTitle = "Update available",
        updateBody = { "NexStatus $it" },
        updateNow = "Update",
        updateLater = "Later",
        updateDownloading = "Downloading…",
        updateFailed = "Couldn't download the update.",
        updateRetry = "Retry",
        myGameHistory = "Play activity",
        allMyGames = "All activity",
        gameHistory = "Play activity",
        historyEmpty = "No play activity yet.",
        historyFailed = "Couldn't load play activity.",
        loadingHistory = "Loading play activity…",
        playHours = ::playHoursEn,
    )
}

private fun playHoursRu(seconds: Long): String {
    val total = seconds.coerceAtLeast(0)
    val hours = total / 3600
    val minutes = (total % 3600) / 60
    return when {
        hours > 0 -> "$hours ч"
        minutes > 0 -> "$minutes мин"
        else -> "0 мин"
    }
}

private fun playHoursEn(seconds: Long): String {
    val total = seconds.coerceAtLeast(0)
    val hours = total / 3600
    val minutes = (total % 3600) / 60
    return when {
        hours > 0 -> if (hours == 1L) "1 h" else "$hours h"
        minutes > 0 -> if (minutes == 1L) "1 min" else "$minutes min"
        else -> "0 min"
    }
}

private fun lobbyStateRu(state: String) = when (state) {
    "searching" -> "Поиск"
    "matched" -> "В матче"
    "full" -> "Полно"
    "open" -> "Открыто"
    else -> state
}

private fun lobbyStateEn(state: String) = when (state) {
    "searching" -> "Searching"
    "matched" -> "In match"
    "full" -> "Full"
    "open" -> "Open"
    else -> state
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

fun cleanText(value: String?): String {
    val text = value?.trim().orEmpty()
    return if (text.isEmpty() || text.equals("null", ignoreCase = true) || text.equals("undefined", ignoreCase = true)) "" else text
}
