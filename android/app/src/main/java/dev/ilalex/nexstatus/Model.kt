package dev.ilalex.nexstatus

import android.app.Application
import android.content.Context
import android.net.Uri
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

data class Game(val name: String, val titleId: String, val players: Int)

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

sealed interface Route {
    data object Home : Route
    data class Game(val game: dev.ilalex.nexstatus.Game) : Route
    data class AboutGame(val game: dev.ilalex.nexstatus.Game) : Route
}

class AppModel(app: Application) : AndroidViewModel(app) {
    private val accessKey = stringPreferencesKey("access")
    private val refreshKey = stringPreferencesKey("refresh")
    private val nameKey = stringPreferencesKey("name")
    private val codeKey = stringPreferencesKey("code")
    private val avatarKey = stringPreferencesKey("avatar")
    private val langKey = stringPreferencesKey("lang")
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
            _session.value = Session(
                prefs[nameKey].orEmpty(),
                prefs[codeKey].orEmpty(),
                avatar = prefs[avatarKey].orEmpty(),
                authed = access.isNotEmpty(),
            )
            if (access.isNotEmpty()) loadProfile()
            else if (pendingState.isNotEmpty()) recoverSignIn()
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
        if (tab == MainTab.Games || tab == MainTab.Account) {
            if (_route.value !is Route.Home) {
                pageJob?.cancel()
                infoJob?.cancel()
                _route.value = Route.Home
                _page.value = null
                _gameInfo.value = GameInfo()
            }
        }
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
                is Route.AboutGame -> openAboutGame(current.game)
                is Route.Game -> openGame(current.game)
                Route.Home -> Unit
            }
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

    fun openAboutGame(game: Game) {
        pageJob?.cancel()
        infoJob?.cancel()
        _gameInfo.value = GameInfo(loading = true)
        _route.value = Route.AboutGame(game)
        infoJob = viewModelScope.launch {
            _gameInfo.value = loadGameInfo(game)
        }
    }

    fun back() {
        when (val current = _route.value) {
            is Route.AboutGame -> {
                infoJob?.cancel()
                _gameInfo.value = GameInfo()
                openGame(current.game)
            }
            is Route.Game -> {
                pageJob?.cancel()
                _page.value = null
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

    private suspend fun refreshHome() {
        _home.update { it.copy(loading = it.games.isEmpty()) }
        val next = withContext(Dispatchers.IO) { runCatching { fetchGames() }.getOrNull() }
        _home.update { current ->
            if (next == null) {
                current.copy(
                    loading = false,
                    error = if (current.games.isEmpty()) copy.loadListFailed else "",
                )
            } else {
                HomeState(
                    games = next,
                    players = next.sumOf { it.players },
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
                val main = authed("https://nextendo.network/api/status/live")
                val extra = authed("https://nextendo.network/service-catalog/v1/status/live")
                buildString {
                    if (main?.code == 200) append(main.body)
                    if (extra?.code == 200) append(extra.body)
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

private fun fetchGames(): List<Game> {
    val response = http("https://nextendo.network/api/online-counts")
    if (response.code != 200) error("http ${response.code}")
    val jeux = JSONObject(response.body).optJSONArray("jeux") ?: JSONArray()
    val games = buildList {
        for (i in 0 until jeux.length()) {
            val item = jeux.optJSONObject(i) ?: continue
            val titles = item.optJSONArray("titres")
            val id = titles?.optString(0).orEmpty()
            add(Game(item.optString("nom"), id, item.optInt("joueurs")))
        }
    }
    return games.sortedWith(compareByDescending<Game> { it.players }.thenBy { it.name })
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
                if (withLobby && item.has("lobby") && !item.isNull("lobby")) add(copy.lobbyLabel(item.optInt("lobby") + 1))
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
