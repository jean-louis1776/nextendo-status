package dev.ilalex.nexstatus

import android.app.Application
import android.content.Context
import android.media.MediaPlayer
import android.net.Uri
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import java.time.LocalTime
import java.time.format.DateTimeFormatter
import java.util.Base64

private val Context.store by preferencesDataStore("nexstatus")

enum class ServerState { Online, Offline, NoStatus }

data class Game(
    val name: String,
    val titleId: String,
    val players: Int,
    val state: ServerState = ServerState.Online,
)

data class Person(val name: String, val mark: String, val extra: String)

data class Lobby(
    val title: String,
    val mode: String,
    val state: String,
    val places: String,
    val members: List<Person>,
)

data class GamePage(val note: String, val lobbies: List<Lobby>, val players: List<Person>)

data class GameInfo(
    val title: String = "",
    val intro: String = "",
    val description: String = "",
    val screenshots: List<String> = emptyList(),
    val loading: Boolean = true,
    val error: String = "",
)

data class HomeState(
    val games: List<Game> = emptyList(),
    val players: Int = 0,
    val updated: String = "",
    val error: String = "",
    val loading: Boolean = true,
)

data class Session(
    val name: String = "",
    val friendCode: String = "",
    val avatar: String = "",
    val status: String = "",
    val authed: Boolean = name.isNotEmpty(),
) {
    val signedIn: Boolean get() = authed || name.isNotEmpty()
}

data class HistoryGame(
    val titleId: String,
    val name: String,
    val seconds: Long,
    val lastPlayed: String = "",
) {
    fun asGame() = Game(name = name, titleId = titleId, players = 0)
}

data class HistoryState(
    val games: List<HistoryGame> = emptyList(),
    val loading: Boolean = false,
    val error: String = "",
)

enum class AboutBack { Lobby, Account, History }

sealed interface Route {
    data object Home : Route
    data class Game(val game: dev.ilalex.nexstatus.Game) : Route
    data class AboutGame(val game: dev.ilalex.nexstatus.Game, val backTo: AboutBack = AboutBack.Lobby) : Route
    data object History : Route
}

class AppModel(app: Application) : AndroidViewModel(app) {
    private val accessKey = stringPreferencesKey("access")
    private val refreshKey = stringPreferencesKey("refresh")
    private val nameKey = stringPreferencesKey("name")
    private val codeKey = stringPreferencesKey("code")
    private val avatarKey = stringPreferencesKey("avatar")
    private val langKey = stringPreferencesKey("lang")
    private val musicKey = booleanPreferencesKey("music")
    private val pendingStateKey = stringPreferencesKey("pending_state")
    private val pendingVerifierKey = stringPreferencesKey("pending_verifier")
    private val gate = Mutex()
    private var access = ""
    private var refresh = ""
    private var signInJob: Job? = null
    private var pageJob: Job? = null
    private var infoJob: Job? = null
    private var pendingState = ""
    private var pendingVerifier = ""
    private var attempt = 0
    private val finishing = java.util.concurrent.atomic.AtomicBoolean(false)

    private val _home = MutableStateFlow(HomeState())
    val home: StateFlow<HomeState> = _home
    private val _session = MutableStateFlow(Session())
    val session: StateFlow<Session> = _session
    private val _route = MutableStateFlow<Route>(Route.Home)
    val route: StateFlow<Route> = _route
    private val _page = MutableStateFlow<GamePage?>(null)
    val page: StateFlow<GamePage?> = _page
    private val _gameInfo = MutableStateFlow(GameInfo())
    val gameInfo: StateFlow<GameInfo> = _gameInfo
    private val _browser = MutableStateFlow<String?>(null)
    val browser: StateFlow<String?> = _browser
    private val _tab = MutableStateFlow(MainTab.Games)
    val tab: StateFlow<MainTab> = _tab
    private val _langChoice = MutableStateFlow(LangChoice.System)
    val langChoice: StateFlow<LangChoice> = _langChoice
    private val _music = MutableStateFlow(true)
    val music: StateFlow<Boolean> = _music
    private var foreground = true
    private var player: MediaPlayer? = null
    private val _history = MutableStateFlow(HistoryState())
    val history: StateFlow<HistoryState> = _history

    val russian: Boolean get() = resolveLang(_langChoice.value)
    val copy: Copy get() = copyFor(russian)

    init {
        viewModelScope.launch {
            val prefs = app.store.data.first()
            access = prefs[accessKey].orEmpty()
            refresh = prefs[refreshKey].orEmpty()
            pendingState = prefs[pendingStateKey].orEmpty()
            pendingVerifier = prefs[pendingVerifierKey].orEmpty()
            _langChoice.value = when (prefs[langKey]) {
                "ru" -> LangChoice.Russian
                "en" -> LangChoice.English
                else -> LangChoice.System
            }
            _music.value = prefs[musicKey] ?: true
            applyMusic()
            _session.value = Session(
                prefs[nameKey].orEmpty(),
                prefs[codeKey].orEmpty(),
                avatar = prefs[avatarKey].orEmpty(),
                authed = access.isNotEmpty(),
            )
            if (access.isNotEmpty()) {
                loadProfile()
                loadHistory()
            } else if (pendingState.isNotEmpty()) recoverSignIn()
        }
        viewModelScope.launch {
            while (isActive) {
                refreshHome()
                delay(15_000)
            }
        }
    }

    fun setTab(tab: MainTab) {
        _tab.value = tab
        if (_route.value !is Route.Home) {
            pageJob?.cancel()
            infoJob?.cancel()
            _route.value = Route.Home
            _page.value = null
            _gameInfo.value = GameInfo()
        }
        if (tab == MainTab.Account && _session.value.signedIn && _history.value.games.isEmpty()) {
            viewModelScope.launch { loadHistory() }
        }
    }

    fun setMusic(on: Boolean) {
        _music.value = on
        applyMusic()
        viewModelScope.launch {
            getApplication<Application>().store.edit { it[musicKey] = on }
        }
    }

    fun setForeground(on: Boolean) {
        foreground = on
        applyMusic()
    }

    private fun applyMusic() {
        val app = getApplication<Application>()
        val track = player ?: MediaPlayer.create(app, R.raw.nexstatus)?.apply { isLooping = true }
        player = track
        if (track == null) return
        val play = _music.value && foreground
        if (play) {
            if (!track.isPlaying) track.start()
        } else if (track.isPlaying) {
            track.pause()
        }
    }

    override fun onCleared() {
        player?.release()
        player = null
        super.onCleared()
    }

    fun setLanguage(choice: LangChoice) {
        _langChoice.value = choice
        viewModelScope.launch {
            getApplication<Application>().store.edit {
                it[langKey] = when (choice) {
                    LangChoice.Russian -> "ru"
                    LangChoice.English -> "en"
                    LangChoice.System -> "system"
                }
            }
            when (val current = _route.value) {
                is Route.AboutGame -> openAboutGame(current.game, current.backTo)
                is Route.Game -> openGame(current.game)
                Route.History, Route.Home -> Unit
            }
        }
    }

    fun openHistory() {
        pageJob?.cancel()
        infoJob?.cancel()
        _route.value = Route.History
        if (_history.value.games.isEmpty()) {
            viewModelScope.launch { loadHistory() }
        }
    }

    fun openGame(game: Game) {
        _page.value = null
        _route.value = Route.Game(game)
        pageJob?.cancel()
        infoJob?.cancel()
        pageJob = viewModelScope.launch {
            while (isActive && _route.value is Route.Game) {
                _page.value = loadPage(game)
                delay(15_000)
            }
        }
    }

    fun openAboutGame(game: Game, backTo: AboutBack = AboutBack.Lobby) {
        pageJob?.cancel()
        infoJob?.cancel()
        _gameInfo.value = GameInfo(loading = true)
        _route.value = Route.AboutGame(game, backTo)
        infoJob = viewModelScope.launch {
            _gameInfo.value = loadGameInfo(game)
        }
    }

    fun back() {
        when (val current = _route.value) {
            is Route.AboutGame -> {
                infoJob?.cancel()
                _gameInfo.value = GameInfo()
                when (current.backTo) {
                    AboutBack.Lobby -> openGame(current.game)
                    AboutBack.Account -> {
                        _tab.value = MainTab.Account
                        _route.value = Route.Home
                    }
                    AboutBack.History -> {
                        _tab.value = MainTab.Account
                        _route.value = Route.History
                    }
                }
            }
            is Route.Game -> {
                pageJob?.cancel()
                _page.value = null
                _route.value = Route.Home
            }
            Route.History -> {
                _tab.value = MainTab.Account
                _route.value = Route.Home
            }
            Route.Home -> Unit
        }
    }

    fun onForeground() {
        val state = pendingState
        val verifier = pendingVerifier
        val id = attempt
        if (state.isEmpty() || access.isNotEmpty()) return
        viewModelScope.launch(Dispatchers.IO) {
            val code = pollCode(state)
            if (code.isNotEmpty() && id == attempt) finishSignIn(code, verifier, id)
        }
    }

    private fun recoverSignIn() {
        val state = pendingState
        val verifier = pendingVerifier
        val id = attempt
        viewModelScope.launch(Dispatchers.IO) {
            val code = pollCode(state)
            val done = code.isNotEmpty() && id == attempt && finishSignIn(code, verifier, id)
            if (!done && id == attempt && access.isEmpty()) {
                clearPending()
                _session.update { it.copy(status = "") }
            }
        }
    }

    fun signIn(context: Context) {
        val id = ++attempt
        signInJob?.cancel()
        signInJob = viewModelScope.launch(Dispatchers.IO) {
            _session.update { it.copy(status = copy.waitingBrowser) }
            try {
                signInBrowser(context, id)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (id == attempt && access.isEmpty()) _session.update { it.copy(status = copy.signInFailed) }
            }
        }
    }

    fun signOut() {
        signInJob?.cancel()
        access = ""
        refresh = ""
        pendingState = ""
        pendingVerifier = ""
        _session.value = Session()
        _page.value = null
        _history.value = HistoryState()
        if (_route.value is Route.History) _route.value = Route.Home
        viewModelScope.launch {
            getApplication<Application>().store.edit {
                it.remove(accessKey)
                it.remove(refreshKey)
                it.remove(nameKey)
                it.remove(codeKey)
                it.remove(avatarKey)
                it.remove(pendingStateKey)
                it.remove(pendingVerifierKey)
            }
        }
    }

    private suspend fun loadHistory() {
        if (access.isEmpty() && refresh.isEmpty()) {
            _history.value = HistoryState()
            return
        }
        _history.update { it.copy(loading = it.games.isEmpty(), error = "") }
        val games = withContext(Dispatchers.IO) {
            gate.withLock {
                val response = authed("https://nextendo.network/api/history")
                if (response?.code != 200 || response.body.isEmpty()) return@withLock null
                runCatching {
                    val array = JSONObject(response.body).optJSONArray("history") ?: JSONArray()
                    buildList {
                        for (i in 0 until array.length()) {
                            val item = array.optJSONObject(i) ?: continue
                            val titleId = item.optString("title_id")
                            val name = item.optString("name")
                            if (titleId.isEmpty() && name.isEmpty()) continue
                            add(
                                HistoryGame(
                                    titleId = titleId,
                                    name = name.ifEmpty { titleId },
                                    seconds = item.optLong("seconds"),
                                    lastPlayed = item.optString("last_played"),
                                ),
                            )
                        }
                    }
                }.getOrNull()
            }
        }
        _history.value = if (games == null) {
            HistoryState(error = copy.historyFailed, loading = false)
        } else {
            HistoryState(games = games, loading = false)
        }
    }

    private suspend fun refreshHome() {
        _home.update { it.copy(loading = it.games.isEmpty()) }
        val token = access
        val next = withContext(Dispatchers.IO) { runCatching { fetchGames(token) }.getOrNull() }
        _home.update { current ->
            if (next == null) {
                current.copy(
                    loading = false,
                    error = if (current.games.isEmpty()) copy.loadListFailed else "",
                )
            } else {
                HomeState(
                    games = next,
                    players = next.sumOf { if (it.state == ServerState.Online) it.players else 0 },
                    updated = LocalTime.now().format(DateTimeFormatter.ofPattern("HH:mm:ss")),
                    loading = false,
                )
            }
        }
    }

    private suspend fun loadProfile() {
        val me = withContext(Dispatchers.IO) {
            gate.withLock {
                val response = authed("https://nextendo.network/api/me") ?: return@withLock null
                if (response.code != 200) return@withLock null
                val json = JSONObject(response.body)
                val account = json.optJSONObject("account") ?: json
                val name = account.optString("username").ifEmpty { account.optString("name") }
                    .ifEmpty { findString(json, "username") }
                val code = account.optString("friend_code").ifEmpty { findString(json, "friend_code") }
                val pidValue = account.opt("pid")
                val pid = when (pidValue) {
                    is Number -> pidValue.toLong().toString()
                    is String -> pidValue
                    null -> ""
                    else -> ""
                }
                val profileResponse = authed("https://nextendo.network/api/profile")
                val image = if (profileResponse?.code == 200) {
                    val body = JSONObject(profileResponse.body)
                    val profile = body.optJSONObject("profile") ?: body
                    profile.optString("image")
                } else {
                    ""
                }
                val picture = image.replace(Regex("\\s"), "")
                val avatar = when {
                    picture.startsWith("http") -> picture
                    picture.startsWith("data:image") -> picture
                    picture.length > 32 && !picture.startsWith("{") && !picture.startsWith("img:") ->
                        "data:image/jpeg;base64,$picture"
                    pid.isNotEmpty() -> "https://nextendo.network/api/avatar?pid=$pid"
                    else -> ""
                }
                Triple(name, code, avatar)
            }
        } ?: return
        if (me.first.isEmpty()) return
        _session.value = Session(me.first, me.second, me.third)
        getApplication<Application>().store.edit {
            it[accessKey] = access
            it[refreshKey] = refresh
            it[nameKey] = me.first
            it[codeKey] = me.second
            it[avatarKey] = me.third
        }
    }

    private suspend fun loadGameInfo(game: Game): GameInfo {
        val lang = if (russian) "ru" else "en"
        val tid = game.titleId.ifEmpty { return GameInfo(loading = false, error = copy.gameInfoFailed) }
        return withContext(Dispatchers.IO) {
            val response = runCatching {
                http("https://api.nlib.cc/nx/$tid?lang=$lang&fields=name,description,intro,screens")
            }.getOrNull()
            if (response?.code != 200 || response.body.isEmpty()) {
                return@withContext GameInfo(loading = false, error = copy.gameInfoFailed)
            }
            val json = runCatching { JSONObject(response.body) }.getOrNull()
                ?: return@withContext GameInfo(loading = false, error = copy.gameInfoFailed)
            val screens = json.optJSONObject("screens")?.optJSONArray("screenshots")
            val shots = buildList {
                if (screens != null) {
                    for (i in 0 until screens.length()) {
                        val url = screens.optString(i).replace("http://", "https://")
                        if (url.isNotEmpty()) add(url)
                    }
                }
            }
            GameInfo(
                title = cleanText(json.optString("name")).ifEmpty { game.name },
                intro = cleanText(json.optString("intro")),
                description = cleanText(json.optString("description")),
                screenshots = shots,
                loading = false,
            )
        }
    }

    private suspend fun loadPage(game: Game): GamePage {
        if (access.isEmpty() && refresh.isEmpty()) {
            return GamePage(copy.signInToSee, emptyList(), emptyList())
        }
        val body = withContext(Dispatchers.IO) {
            gate.withLock {
                fun ok(url: String) = authed(url)?.takeIf { it.code == 200 }?.body.orEmpty()
                val id = game.titleId
                when {
                    id in DETAILED -> ok("https://nextendo.network/service-catalog/v1/status/live")
                    id == KIRBY -> ok("https://nextendo.network/game-stats/kirby-live")
                    id == UNO -> ok("https://nextendo.network/game-stats/uno-live")
                    id == WONDER -> {
                        val wonder = ok("https://nextendo.network/game-stats/wonder-live")
                        if (extractGame(wonder, id) != null) wonder else ok("https://nextendo.network/api/status/live")
                    }
                    else -> ok("https://nextendo.network/api/status/live")
                }
            }
        }
        if (body.isEmpty()) {
            return GamePage(
                if (game.players > 0) "${copy.playersOnlineNote(game.players)} ${copy.listUnavailable}"
                else copy.loadLobbiesFailed,
                emptyList(),
                emptyList(),
            )
        }
        return describe(body, game, copy)
    }

    private suspend fun signInBrowser(context: Context, id: Int): Boolean {
        val random = SecureRandom()
        val alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
        val pair = CharArray(6) { alphabet[random.nextInt(alphabet.length)] }.concatToString()
        val state = ByteArray(16).also(random::nextBytes).joinToString("") { "%02x".format(it) }
        val verifier = CharArray(64) { PKCE[random.nextInt(PKCE.length)] }.concatToString()
        val digest = MessageDigest.getInstance("SHA-256").digest(verifier.toByteArray(Charsets.US_ASCII))
        val challenge = Base64.getUrlEncoder().withoutPadding().encodeToString(digest)
        val posted = withContext(Dispatchers.IO) {
            http(
                "$RELAY/pair",
                "POST",
                """{"code":"$pair","state":"$state","challenge":"$challenge"}""",
                mapOf("Content-Type" to "application/json"),
            ).code == 200
        }
        if (!posted) {
            if (id == attempt) _session.update { it.copy(status = copy.signInOpenFailed) }
            return false
        }
        if (id != attempt) return false
        pendingState = state
        pendingVerifier = verifier
        getApplication<Application>().store.edit {
            it[pendingStateKey] = state
            it[pendingVerifierKey] = verifier
        }
        withContext(Dispatchers.Main) { _browser.value = "$RELAY/go?code=$pair" }
        val deadline = System.currentTimeMillis() + 5 * 60_000
        while (currentCoroutineContext().isActive && System.currentTimeMillis() < deadline && access.isEmpty()) {
            val code = pollCode(state)
            if (code.isNotEmpty()) return finishSignIn(code, verifier, id)
            delay(1_000)
        }
        if (access.isNotEmpty()) return true
        if (id != attempt) return false
        withContext(Dispatchers.Main) { _browser.value = null }
        clearPending()
        _session.update { it.copy(status = copy.signInTimedOut) }
        return false
    }

    fun onBrowserCallback(url: String) {
        val uri = Uri.parse(url)
        val code = uri.getQueryParameter("code").orEmpty()
        val state = uri.getQueryParameter("state").orEmpty()
        _browser.value = null
        if (state != pendingState || code.length < 8) {
            if (access.isEmpty()) _session.update { it.copy(status = copy.signInFailed) }
            return
        }
        val id = attempt
        val verifier = pendingVerifier
        viewModelScope.launch(Dispatchers.IO) { finishSignIn(code, verifier, id) }
    }

    fun cancelBrowser() {
        _browser.value = null
        attempt++
        signInJob?.cancel()
        viewModelScope.launch {
            clearPending()
            if (access.isEmpty()) _session.update { it.copy(status = "") }
        }
    }

    private suspend fun clearPending() {
        pendingState = ""
        pendingVerifier = ""
        getApplication<Application>().store.edit {
            it.remove(pendingStateKey)
            it.remove(pendingVerifierKey)
        }
    }

    private fun pollCode(state: String): String {
        val response = runCatching { http("$RELAY/poll?state=$state") }.getOrNull() ?: return ""
        if (response.code != 200 || response.body.isBlank() || !response.body.trimStart().startsWith("{")) return ""
        return runCatching { JSONObject(response.body).optString("code") }.getOrDefault("")
    }

    private suspend fun finishSignIn(code: String, verifier: String, id: Int): Boolean {
        if (id != attempt) return false
        if (!finishing.compareAndSet(false, true)) return access.isNotEmpty()
        try {
            if (id != attempt) return false
            val tokens = exchange(code, verifier)
            if (id != attempt) return false
            if (tokens == null) {
                clearPending()
                _session.update { it.copy(status = copy.signInFailed) }
                return false
            }
            access = tokens.first
            refresh = tokens.second
            withContext(Dispatchers.Main) { _browser.value = null }
            clearPending()
            getApplication<Application>().store.edit {
                it[accessKey] = access
                it[refreshKey] = refresh
            }
            _session.update { it.copy(authed = true, status = "") }
            loadProfile()
            if (_session.value.name.isEmpty()) {
                _session.update { it.copy(authed = true, status = copy.profilePartial) }
            }
            loadHistory()
            refreshOpenGame()
            return true
        } finally {
            finishing.set(false)
        }
    }

    private fun refreshOpenGame() {
        val game = (_route.value as? Route.Game)?.game ?: return
        pageJob?.cancel()
        pageJob = viewModelScope.launch {
            _page.value = null
            while (isActive && _route.value is Route.Game) {
                _page.value = loadPage(game)
                delay(15_000)
            }
        }
    }

    private fun exchange(code: String, verifier: String): Pair<String, String>? {
        val form = "grant_type=authorization_code" +
            "&client_id=${enc(CLIENT)}" +
            "&code=${enc(code)}" +
            "&redirect_uri=${enc("$RELAY/callback")}" +
            "&code_verifier=${enc(verifier)}"
        val response = http(
            "https://nextendo.network/oauth/token",
            "POST",
            form,
            mapOf("Content-Type" to "application/x-www-form-urlencoded"),
        )
        if (response.code !in 200..299) return null
        val json = JSONObject(response.body)
        val accessToken = json.optString("access_token")
        if (accessToken.isEmpty()) return null
        return accessToken to json.optString("refresh_token")
    }

    private fun authed(url: String): HttpResult? {
        if (access.isEmpty() && !renew()) return null
        var response = http(url, headers = authHeaders(access))
        if (response.code == 401 && renew()) response = http(url, headers = authHeaders(access))
        return response
    }

    private fun renew(): Boolean {
        if (refresh.isEmpty()) return false
        val form = "grant_type=refresh_token&client_id=${enc(CLIENT)}&refresh_token=${enc(refresh)}"
        val response = http(
            "https://nextendo.network/oauth/token",
            "POST",
            form,
            mapOf("Content-Type" to "application/x-www-form-urlencoded"),
        )
        if (response.code !in 200..299) return false
        val json = JSONObject(response.body)
        val next = json.optString("access_token")
        if (next.isEmpty()) return false
        access = next
        val rotated = json.optString("refresh_token")
        if (rotated.isNotEmpty()) refresh = rotated
        return true
    }
}

private const val RELAY = "https://d5de3i1ormj1kpci9q3d.764nr5vy.apigw.yandexcloud.net"
private const val CLIENT = "nxc_dyNRS0L9SLND"
private const val PKCE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"

private data class HttpResult(val code: Int, val body: String)

private fun authHeaders(token: String) = mapOf(
    "Authorization" to "Bearer $token",
    "X-Nextendo-Client-Id" to CLIENT,
)

private fun enc(value: String) = URLEncoder.encode(value, "UTF-8")

private fun http(
    url: String,
    method: String = "GET",
    body: String? = null,
    headers: Map<String, String> = emptyMap(),
): HttpResult {
    val connection = (URL(url).openConnection() as HttpURLConnection).apply {
        requestMethod = method
        connectTimeout = 15_000
        readTimeout = 40_000
        instanceFollowRedirects = true
        setRequestProperty("User-Agent", "NexStatus")
        headers.forEach { (key, value) -> setRequestProperty(key, value) }
        if (body != null) {
            doOutput = true
            outputStream.use { it.write(body.toByteArray()) }
        }
    }
    return try {
        val code = connection.responseCode
        val stream = if (code in 200..299) connection.inputStream else connection.errorStream
        HttpResult(code, stream?.let { readCap(it) }.orEmpty())
    } finally {
        connection.disconnect()
    }
}

private fun readCap(stream: InputStream): String {
    val out = ByteArrayOutputStream()
    val buf = ByteArray(8192)
    var total = 0
    val cap = 16 * 1024 * 1024
    stream.use {
        while (total < cap) {
            val n = it.read(buf, 0, minOf(buf.size, cap - total))
            if (n < 0) break
            out.write(buf, 0, n)
            total += n
        }
    }
    return out.toString(Charsets.UTF_8.name())
}

private class CatalogTitle(val name: String, val ids: String, val nostatus: Boolean = false)

// Same 63 games as https://nextendo.network/status, including servers that are down.
private val CATALOG = listOf(
    CatalogTitle("SpeedRunners 2: King of Speed", "01004cc023582000", nostatus = true),
    CatalogTitle("SEGA Genesis - Nintendo Classics", "0100b3c014bda000"),
    CatalogTitle("Just Dance 2022", "0100ea6014bb8000"),
    CatalogTitle("Mario Kart 8 Deluxe", "0100152000022000"),
    CatalogTitle("Splatoon 2", "0100f8f0000a2000,01003bc0000a0000,01003c700009c800"),
    CatalogTitle("Splatoon 3", "0100c2500fc20000"),
    CatalogTitle("Super Smash Bros. Ultimate", "01006a800016e000"),
    CatalogTitle("Animal Crossing: New Horizons", "01006f8002326000"),
    CatalogTitle("Luigi's Mansion 3", "0100dca0064a6000"),
    CatalogTitle("Minecraft: Nintendo Switch Edition", "01006bd001e06000"),
    CatalogTitle("ARMS", "01009b500007c000"),
    CatalogTitle("Mario Tennis Aces", "0100bde00862a000"),
    CatalogTitle("Super Mario Maker 2", "01009b90006dc000"),
    CatalogTitle("PAC-MAN 99", "0100ad9012510000"),
    CatalogTitle("Among Us", "0100b0c013912000"),
    CatalogTitle("Mario & Sonic at the Olympic Games Tokyo 2020", "010086000e148000"),
    CatalogTitle("Tetris 99", "010040600c5ce000"),
    CatalogTitle("Super Mario Bros. 35", "0100277011f1a000"),
    CatalogTitle("Super Mario Bros. Wonder", "010015100b514000"),
    CatalogTitle("Clubhouse Games: 51 Worldwide Classics", "010047700d540000"),
    CatalogTitle("Monster Hunter Generations Ultimate", "0100770008dd8000"),
    CatalogTitle("Crash Team Racing Nitro-Fueled", "0100f9f00c696000"),
    CatalogTitle("Diablo III: Eternal Collection", "01001b300b9be000"),
    CatalogTitle("Mario Party Superstars", "01006fe013472000"),
    CatalogTitle("Super Mario Party Jamboree", "0100965017338000"),
    CatalogTitle("Kirby's Dream Buffet", "0100a8e016236000"),
    CatalogTitle("Stardew Valley", "0100e65002bb8000"),
    CatalogTitle("Overcooked! 2", "01006fd0080b2000"),
    CatalogTitle("Pokémon Scarlet and Violet", "0100a3d008c5c000,01008f6008c5e000"),
    CatalogTitle("Nintendo 64 - Nintendo Classics", "0100c9a00ece6000"),
    CatalogTitle("NES - Nintendo Classics", "0100d870045b6000"),
    CatalogTitle("SNES - Nintendo Classics", "01008d300c50c000"),
    CatalogTitle("Game Boy - Nintendo Classics", "0100c62011050000"),
    CatalogTitle("Game Boy Advance - Nintendo Classics", "010012f017576000"),
    CatalogTitle("Pokémon Legends: Z-A", "0100f43008c44000"),
    CatalogTitle("Pokémon Sword and Shield", "0100abf008968000,01008db008c2c000"),
    CatalogTitle("Super Monkey Ball Banana Mania", "010001701248c000"),
    CatalogTitle("Mario Golf: Super Rush", "0100c9c00e25c000"),
    CatalogTitle("Super Mario 3D World + Bowser's Fury", "010028600ebda000"),
    CatalogTitle("Super Mario Odyssey", "0100000000010000"),
    CatalogTitle("METAL GEAR SOLID: Peace Walker", "0100c6f01c4f8000"),
    CatalogTitle("Mario Strikers: Battle League", "010019401051c000"),
    CatalogTitle("Minecraft Dungeons II", "0100a7c01b792000"),
    CatalogTitle("Pokkén Tournament DX", "0100b3f000be2000"),
    CatalogTitle("Saints Row: The Third - The Full Package", "0100de600beee000"),
    CatalogTitle("UNO", "0100b6e012ebe000"),
    CatalogTitle("Dying Light: Platinum Edition", "01008c8012920000"),
    CatalogTitle("Guilty Gear -Strive-", "0100ab401f136000"),
    CatalogTitle("Kirby Fighters 2", "0100227010460000"),
    CatalogTitle("Terraria", "0100e46006708000"),
    CatalogTitle("Goat Simulator 3", "01001cc01b2d4000"),
    CatalogTitle("Super Kirby Clash", "01003fb00c5a8000"),
    CatalogTitle("Ultimate Chicken Horse", "0100fcf002a58000", nostatus = true),
    CatalogTitle("Crypt of the NecroDancer", "0100cea007d08000", nostatus = true),
    CatalogTitle("Nintendo Switch Sports", "0100d2f00d5c0000"),
    CatalogTitle("Monopoly for Nintendo Switch", "01007430037f6000"),
    CatalogTitle("Dragon Ball FighterZ", "0100a250097f0000"),
    CatalogTitle("Friday the 13th: The Game", "010092a00c4b6000"),
    CatalogTitle("Monster Hunter Rise", "0100b04011742000"),
    CatalogTitle("Miitopia", "01003da010e8a000"),
    CatalogTitle("Portal 2", "0100abd01785c000"),
    CatalogTitle("Game Builder Garage", "0100fa5010788000"),
    CatalogTitle("Zombie Army 4: Dead War", "01000bf0152fa000"),
)

private const val SM3DW = "010028600ebda000"
private const val JAMBOREE = "0100965017338000"
private const val KIRBY = "0100a8e016236000"
private const val JD = "0100ea6014bb8000"
private const val UNO = "0100b6e012ebe000"
private const val CRYPT = "0100cea007d08000"
private const val SR2 = "01004cc023582000"
private const val WONDER = "010015100b514000"

// Catalog games whose public count and, once signed in, player list come from the service catalog.
private val CLASSICS = setOf(
    "0100d870045b6000", "01008d300c50c000", "0100c62011050000", "010012f017576000",
    "0100b3c014bda000", "0100fcf002a58000", "0100ab401f136000", "01003fb00c5a8000",
    "0100227010460000", "0100e46006708000", "01001cc01b2d4000", "0100e65002bb8000",
)
private val DETAILED = CLASSICS + JD

private fun jsonObject(url: String, headers: Map<String, String> = emptyMap()): JSONObject? {
    val response = http(url, headers = headers)
    if (response.code != 200 || response.body.isEmpty()) return null
    return runCatching { JSONObject(response.body) }.getOrNull()
}

private fun numbered(json: JSONObject?, key: String): Int? {
    if (json == null || !json.has(key) || json.isNull(key)) return null
    val value = json.opt(key)
    return if (value is Number) value.toInt() else null
}

private fun fetchGames(token: String): List<Game> {
    val response = http("https://nextendo.network/api/online-counts")
    if (response.code != 200) error("http ${response.code}")
    val counts = mutableMapOf<String, Int>()
    val raw = JSONObject(response.body).optJSONObject("counts")
    if (raw != null) {
        val keys = raw.keys()
        while (keys.hasNext()) {
            val key = keys.next()
            val value = raw.opt(key)
            if (value is Number) counts[key] = value.toInt()
        }
    }
    val avail = mutableMapOf<String, Boolean>()
    numbered(jsonObject("https://nextendo.network/game-stats/sm3dw"), "connected")?.let { counts[SM3DW] = it }
    counts.remove(JAMBOREE)
    jsonObject("https://nextendo.network/game-stats/jamboree")?.let { stats ->
        val n = numbered(stats, "connected")
        if (stats.optBoolean("online") && n != null) counts[JAMBOREE] = n
    }
    counts.remove(KIRBY)
    jsonObject("https://nextendo.network/game-stats/kirby")?.let { stats ->
        val n = numbered(stats, "connected")
        if (stats.optBoolean("online") && n != null) counts[KIRBY] = n
    }
    counts.remove(JD)
    avail[JD] = false
    jsonObject("https://nextendo.network/game-stats/justdance22")?.let { stats ->
        val n = numbered(stats, "connected")
        if (stats.optString("game") == "Just Dance 2022" && stats.optString("titleId") == JD &&
            stats.optBoolean("online") && n != null && n >= 0
        ) {
            counts[JD] = n
            avail[JD] = true
        }
    }
    val catalog = jsonObject("https://nextendo.network/service-catalog/v1/online-counts")?.optJSONObject("counts")
    for (id in CLASSICS) {
        counts.remove(id)
        avail[id] = false
        val n = numbered(catalog, id)
        if (n != null && n >= 0) {
            counts[id] = n
            avail[id] = true
        }
    }
    jsonObject("https://nextendo.network/game-stats/uno-count")?.let { stats ->
        val n = numbered(stats, "connected")
        if (stats.optString("game") == "UNO" && stats.optString("titleId") == UNO && n != null && n >= 0) {
            counts[UNO] = n
            avail[UNO] = true
        }
    }
    counts.remove(CRYPT)
    avail[CRYPT] = false
    jsonObject("https://nextendo.network/service-catalog/v1/crypt-status")?.let { stats ->
        if (stats.optString("game") == "Crypt of the NecroDancer" && stats.optString("titleId") == CRYPT &&
            stats.optBoolean("online") && stats.optString("countScope") == "room_members"
        ) {
            avail[CRYPT] = true
            val n = numbered(stats, "players")
            if (n != null && n in 0..1024) counts[CRYPT] = n
        }
    }
    counts.remove(SR2)
    avail[SR2] = false
    jsonObject("https://nextendo.network/service-catalog/v1/sr2-status")?.let { stats ->
        if (stats.optString("game") == "SpeedRunners 2: King of Speed" && stats.optString("titleId") == SR2 &&
            stats.optBoolean("online") && stats.optString("countScope") == "native_transport_connections"
        ) {
            avail[SR2] = true
            val n = numbered(stats, "players")
            if (n != null && n in 0..50000) counts[SR2] = n
        }
    }
    if (token.isNotEmpty()) {
        val live = http(
            "https://nextendo.network/service-catalog/v1/status/live",
            headers = authHeaders(token),
        )
        if (live.code == 200) applyLiveCounts(live.body, counts, avail)
    }
    return CATALOG.map { title ->
        var best = 0
        var found = false
        var up = false
        for (id in title.ids.split(',')) {
            val n = counts[id]
            if (n != null) {
                found = true
                if (n > best) best = n
            }
            if (avail[id] == true) up = true
        }
        val online = found || up
        val state = when {
            online -> ServerState.Online
            title.nostatus -> ServerState.NoStatus
            else -> ServerState.Offline
        }
        Game(title.name, title.ids.substringBefore(','), if (found) best else 0, state)
    }.sortedWith(compareByDescending<Game> { it.players }.thenBy { it.name })
}

private fun applyLiveCounts(body: String, counts: MutableMap<String, Int>, avail: MutableMap<String, Boolean>) {
    val games = runCatching { JSONObject(body).optJSONArray("games") }.getOrNull() ?: return
    for (i in 0 until games.length()) {
        val game = games.optJSONObject(i) ?: continue
        val titles = game.optJSONArray("titles") ?: continue
        if (titles.length() != 1) continue
        val id = titles.optString(0)
        if (id !in DETAILED) continue
        val players = game.optJSONArray("players") ?: continue
        if (game.optJSONArray("lobbies") == null) continue
        counts[id] = players.length()
        avail[id] = true
    }
}

private fun describe(body: String, game: Game, copy: Copy): GamePage {
    val slice = extractGame(body, game.titleId)
    if (slice == null) {
        return GamePage(
            if (game.players > 0) "${copy.playersOnlineNote(game.players)} ${copy.listUnavailable}"
            else copy.nobodyHere,
            emptyList(),
            emptyList(),
        )
    }
    val json = JSONObject(slice)
    val players = people(json.optJSONArray("players"), withLobby = true, copy)
    if (players.isEmpty()) {
        return GamePage(
            if (game.players > 0) "${copy.playersOnlineNote(game.players)} ${copy.listUnavailable}"
            else copy.nobodyHere,
            emptyList(),
            emptyList(),
        )
    }
    val roomsHidden = json.opt("roomsAvailable") == false
    val lobbyJson = json.optJSONArray("lobbies")
    val lobbies = if (roomsHidden || lobbyJson == null) emptyList() else buildList {
        for (i in 0 until lobbyJson.length()) {
            val item = lobbyJson.optJSONObject(i) ?: continue
            val count = if (item.has("count")) item.optInt("count") else -1
            val max = if (item.has("max")) item.optInt("max") else -1
            val places = when {
                count >= 0 && max > 0 -> "$count/$max"
                count >= 0 -> count.toString()
                else -> ""
            }
            add(
                Lobby(
                    title = copy.lobbyTitle(i + 1),
                    mode = cleanText(item.optString("mode")),
                    state = copy.lobbyState(item.optString("state")),
                    places = places,
                    members = people(item.optJSONArray("players"), withLobby = false, copy),
                ),
            )
        }
    }
    val note = when {
        roomsHidden -> copy.roomsUnavailable
        lobbies.isEmpty() -> copy.onlineNotInRoom
        else -> ""
    }
    return GamePage(note, lobbies, players)
}

private fun people(array: JSONArray?, withLobby: Boolean, copy: Copy): List<Person> {
    if (array == null) return emptyList()
    return buildList {
        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue
            val name = item.optString("name")
            if (name.isEmpty()) continue
            val mark = when {
                item.optBoolean("you") -> copy.markYou
                item.optBoolean("friend") -> copy.markFriend
                else -> ""
            }
            val extra = buildList {
                if (withLobby && item.has("lobby") && !item.isNull("lobby")) {
                    val lobby = item.optInt("lobby", -1)
                    if (lobby >= 0) add(copy.lobbyLabel(lobby + 1))
                }
                val activity = cleanText(item.optString("activity"))
                if (activity.isNotEmpty()) add(activity)
            }.joinToString("  ")
            add(Person(name, mark, extra))
        }
    }
}

private fun findString(json: JSONObject, key: String): String {
    val direct = json.opt(key)
    if (direct is String && direct.isNotEmpty()) return direct
    val keys = json.keys()
    while (keys.hasNext()) {
        val value = json.opt(keys.next())
        if (value is JSONObject) {
            val found = findString(value, key)
            if (found.isNotEmpty()) return found
        }
    }
    return ""
}

private fun extractGame(body: String, titleId: String): String? {
    if (titleId.isEmpty()) return null
    val needle = "\"$titleId\""
    val opens = ArrayDeque<Int>()
    val hits = ArrayList<Int>()
    var str = false
    var esc = false
    var i = 0
    while (i < body.length) {
        val c = body[i]
        if (str) {
            if (esc) esc = false
            else if (c == '\\') esc = true
            else if (c == '"') str = false
        } else if (c == '"') {
            if (opens.isNotEmpty() && body.startsWith(needle, i)) hits.add(opens.last())
            str = true
        } else if (c == '{') {
            opens.addLast(i)
        } else if (c == '}' && opens.isNotEmpty()) {
            opens.removeLast()
        }
        i++
    }
    var best: String? = null
    var bestNames = -1
    for (open in hits) {
        val close = matchClose(body, open) ?: continue
        val game = body.substring(open, close + 1)
        var names = 0
        var at = 0
        while (true) {
            at = game.indexOf("\"name\"", at)
            if (at < 0) break
            names++
            at += 6
        }
        if (names > bestNames) {
            bestNames = names
            best = game
        }
    }
    return best
}

private fun matchClose(body: String, open: Int): Int? {
    val close = if (body[open] == '{') '}' else ']'
    var depth = 0
    var str = false
    var esc = false
    var i = open
    while (i < body.length) {
        val c = body[i]
        if (str) {
            if (esc) esc = false
            else if (c == '\\') esc = true
            else if (c == '"') str = false
        } else if (c == '"') {
            str = true
        } else if (c == '{' || c == '[') {
            depth++
        } else if (c == '}' || c == ']') {
            depth--
            if (depth == 0) return if (c == close) i else null
        }
        i++
    }
    return null
}
