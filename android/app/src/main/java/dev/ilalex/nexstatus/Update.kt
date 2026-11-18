package dev.ilalex.nexstatus

import android.app.Application
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.content.FileProvider
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

private const val UPDATE_REPO = "jean-louis1776/nextendo-status"

sealed interface AppUpdate {
    data object Idle : AppUpdate
    data class Available(val version: String, val apkUrl: String) : AppUpdate
    data class Downloading(
        val version: String,
        val apkUrl: String,
        val got: Long,
        val total: Long,
        val speed: Int,
    ) : AppUpdate
    data class Failed(val version: String, val apkUrl: String) : AppUpdate
}

fun versionParts(text: String): List<Int> {
    var s = text.trim()
    if (s.startsWith("v", ignoreCase = true)) s = s.drop(1)
    val parts = mutableListOf<Int>()
    var i = 0
    while (i < s.length && parts.size < 3) {
        var n = 0
        var any = false
        while (i < s.length && s[i] in '0'..'9') {
            n = n * 10 + (s[i] - '0')
            i++
            any = true
        }
        if (!any) break
        parts += n
        if (i < s.length && s[i] == '.') i++ else break
    }
    while (parts.size < 3) parts += 0
    return parts
}

fun isNewerVersion(remote: String, current: String): Boolean {
    val a = versionParts(remote)
    val b = versionParts(current)
    for (i in 0 until 3) {
        if (a[i] != b[i]) return a[i] > b[i]
    }
    return false
}

fun downloadStats(got: Long, total: Long, bytesPerSec: Int): String {
    val pct = if (total > 0) ((got * 100) / total).toInt().coerceIn(0, 100) else 0
    val rate = when {
        bytesPerSec >= 1024 * 1024 -> "%.1f MB/s".format(bytesPerSec / (1024.0 * 1024.0))
        bytesPerSec >= 1024 -> "${bytesPerSec / 1024} KB/s"
        bytesPerSec > 0 -> "$bytesPerSec B/s"
        else -> "0 B/s"
    }
    return "$pct%  $rate"
}

class UpdateModel(app: Application) : AndroidViewModel(app) {
    private val _state = MutableStateFlow<AppUpdate>(AppUpdate.Idle)
    val state: StateFlow<AppUpdate> = _state
    private var job: Job? = null

    fun check(currentVersion: String) {
        if (_state.value !is AppUpdate.Idle) return
        viewModelScope.launch(Dispatchers.IO) {
            val found = findUpdate(currentVersion) ?: return@launch
            _state.value = AppUpdate.Available(found.first, found.second)
        }
    }

    fun later() {
        if (_state.value is AppUpdate.Downloading) return
        _state.value = AppUpdate.Idle
    }

    fun startDownload(context: Context) {
        val current = _state.value
        val version: String
        val url: String
        when (current) {
            is AppUpdate.Available -> {
                version = current.version
                url = current.apkUrl
            }
            is AppUpdate.Failed -> {
                version = current.version
                url = current.apkUrl
            }
            else -> return
        }
        job?.cancel()
        job = viewModelScope.launch(Dispatchers.IO) {
            _state.value = AppUpdate.Downloading(version, url, 0, 0, 0)
            val file = File(context.cacheDir, "updates").apply { mkdirs() }.let { File(it, "nexstatus.apk") }
            val ok = downloadApk(url, file) { got, total, speed ->
                if (isActive) _state.value = AppUpdate.Downloading(version, url, got, total, speed)
            }
            if (!isActive) return@launch
            if (ok && file.exists() && file.length() > 0) {
                withContext(Dispatchers.Main) {
                    installApk(context, file)
                    _state.value = AppUpdate.Idle
                }
            } else {
                _state.value = AppUpdate.Failed(version, url)
            }
        }
    }

    private fun findUpdate(current: String): Pair<String, String>? {
        val response = runCatching {
            httpGet("https://api.github.com/repos/$UPDATE_REPO/releases/latest")
        }.getOrNull() ?: return null
        if (response.first !in 200..299 || response.second.isEmpty()) return null
        val json = runCatching { JSONObject(response.second) }.getOrNull() ?: return null
        val assets = json.optJSONArray("assets") ?: return null
        var apkUrl = ""
        var remoteVersion = ""
        for (i in 0 until assets.length()) {
            val asset = assets.optJSONObject(i) ?: continue
            val name = asset.optString("name")
            val url = asset.optString("browser_download_url")
            when {
                name.equals("nexstatus.apk", ignoreCase = true) -> apkUrl = url
                name.equals("android-version.txt", ignoreCase = true) -> {
                    remoteVersion = runCatching {
                        httpGet(url).second.trim()
                    }.getOrDefault("")
                }
            }
        }
        if (apkUrl.isEmpty() || remoteVersion.isEmpty()) return null
        if (!isNewerVersion(remoteVersion, current)) return null
        return remoteVersion to apkUrl
    }

    private fun downloadApk(url: String, dest: File, onProgress: (Long, Long, Int) -> Unit): Boolean {
        dest.parentFile?.mkdirs()
        if (dest.exists()) dest.delete()
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 15_000
            readTimeout = 60_000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "NexStatus-Android")
            setRequestProperty("Accept", "application/octet-stream")
        }
        return try {
            if (connection.responseCode !in 200..299) return false
            val total = connection.contentLengthLong.coerceAtLeast(0L)
            var got = 0L
            var markBytes = 0L
            var markAt = System.nanoTime()
            var speed = 0
            connection.inputStream.use { input ->
                dest.outputStream().use { output ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        output.write(buf, 0, n)
                        got += n
                        val now = System.nanoTime()
                        val dt = now - markAt
                        if (dt >= 250_000_000L) {
                            val delta = got - markBytes
                            speed = ((delta * 1_000_000_000.0) / dt).toInt().coerceAtLeast(0)
                            markBytes = got
                            markAt = now
                        }
                        onProgress(got, total, speed)
                    }
                }
            }
            onProgress(got, if (total > 0) total else got, speed)
            got > 0
        } catch (_: Exception) {
            dest.delete()
            false
        } finally {
            connection.disconnect()
        }
    }

    private fun installApk(context: Context, file: File) {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.files", file)
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            if (Build.VERSION.SDK_INT < 24) {
                // no-op: FileProvider covers modern targets
            }
        }
        context.startActivity(intent)
    }

    private fun httpGet(url: String): Pair<Int, String> {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 15_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "NexStatus-Android")
            setRequestProperty("Accept", "application/vnd.github+json")
        }
        return try {
            val code = connection.responseCode
            val stream = if (code in 200..299) connection.inputStream else connection.errorStream
            val body = stream?.bufferedReader()?.use { it.readText() }.orEmpty()
            code to body
        } finally {
            connection.disconnect()
        }
    }
}
