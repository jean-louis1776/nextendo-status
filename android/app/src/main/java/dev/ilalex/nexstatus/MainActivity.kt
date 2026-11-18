package dev.ilalex.nexstatus

import android.content.res.Configuration
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.LocalOverscrollFactory
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.WindowCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.chrisbanes.haze.rememberHazeState

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        val splash = installSplashScreen()
        splash.setKeepOnScreenCondition { false }
        splash.setOnExitAnimationListener { screen -> screen.remove() }
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            var boot by remember { mutableStateOf(false) }
            LaunchedEffect(Unit) {
                boot = true
                reportFullyDrawn()
            }
            if (!boot) {
                Box(Modifier.fillMaxSize().background(Color(0xFF111113)))
                return@setContent
            }
            val model: AppModel = viewModel()
            val updates: UpdateModel = viewModel()
            val dark = true
            SideEffect {
                val controller = WindowCompat.getInsetsController(window, window.decorView)
                controller.isAppearanceLightStatusBars = !dark
                controller.isAppearanceLightNavigationBars = !dark
            }
            val base = LocalConfiguration.current
            val configuration = remember(base, dark) {
                Configuration(base).apply {
                    val night = if (dark) Configuration.UI_MODE_NIGHT_YES else Configuration.UI_MODE_NIGHT_NO
                    uiMode = (uiMode and Configuration.UI_MODE_NIGHT_MASK.inv()) or night
                }
            }
            CompositionLocalProvider(LocalConfiguration provides configuration) {
                NexTheme(dark) {
                    val haze = rememberHazeState()
                    val glass = rememberGlass(dark)
                    val home by model.home.collectAsStateWithLifecycle()
                    val session by model.session.collectAsStateWithLifecycle()
                    val route by model.route.collectAsStateWithLifecycle()
                    val page by model.page.collectAsStateWithLifecycle()
                    val gameInfo by model.gameInfo.collectAsStateWithLifecycle()
                    val browser by model.browser.collectAsStateWithLifecycle()
                    val tab by model.tab.collectAsStateWithLifecycle()
                    val langChoice by model.langChoice.collectAsStateWithLifecycle()
                    val update by updates.state.collectAsStateWithLifecycle()
                    val copy = remember(langChoice) { copyFor(resolveLang(langChoice)) }
                    val context = LocalContext.current
                    val appVersion = remember {
                        runCatching {
                            context.packageManager.getPackageInfo(context.packageName, 0).versionName.orEmpty()
                        }.getOrDefault("0")
                    }
                    LaunchedEffect(appVersion) { updates.check(appVersion) }
                    val lifecycleOwner = LocalLifecycleOwner.current
                    DisposableEffect(lifecycleOwner, model) {
                        val observer = LifecycleEventObserver { _, event ->
                            if (event == Lifecycle.Event.ON_START) model.onForeground()
                        }
                        lifecycleOwner.lifecycle.addObserver(observer)
                        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
                    }
                    BackHandler(enabled = browser != null || route !is Route.Home || update !is AppUpdate.Idle) {
                        when {
                            browser != null -> model.cancelBrowser()
                            update is AppUpdate.Downloading -> Unit
                            update !is AppUpdate.Idle -> updates.later()
                            else -> model.back()
                        }
                    }
                    var browserPage by remember { mutableStateOf<String?>(null) }
                    if (browser != null) browserPage = browser
                    CompositionLocalProvider(LocalOverscrollFactory provides null) {
                        Box(Modifier.fillMaxSize()) {
                            Stage(haze) {
                                AnimatedContent(
                                    targetState = route to tab,
                                    modifier = Modifier.fillMaxSize(),
                                    transitionSpec = {
                                        val goingDeeper = targetState.first is Route.Game || targetState.first is Route.AboutGame
                                        val comingBack = initialState.first !is Route.Home && targetState.first is Route.Home
                                        when {
                                            goingDeeper && !comingBack ->
                                                (fadeIn(tween(240)) + slideInHorizontally(tween(240)) { it / 6 }) togetherWith
                                                    (fadeOut(tween(160)) + slideOutHorizontally(tween(160)) { -it / 10 })
                                            comingBack || targetState.first is Route.Home && initialState.first !is Route.Home ->
                                                (fadeIn(tween(240)) + slideInHorizontally(tween(240)) { -it / 10 }) togetherWith
                                                    (fadeOut(tween(160)) + slideOutHorizontally(tween(160)) { it / 6 })
                                            else -> fadeIn(tween(180)) togetherWith fadeOut(tween(120))
                                        }
                                    },
                                    contentKey = { (r, t) ->
                                        when (r) {
                                            Route.Home -> "home-$t"
                                            is Route.Game -> "game-${r.game.titleId}"
                                            is Route.AboutGame -> "about-${r.game.titleId}"
                                        }
                                    },
                                    label = "screen",
                                ) { (current, currentTab) ->
                                    when (current) {
                                        Route.Home -> when (currentTab) {
                                            MainTab.Games -> HomeScreen(home, copy, haze, glass, model::openGame)
                                            MainTab.Account -> AccountScreen(
                                                session,
                                                copy,
                                                langChoice,
                                                haze,
                                                glass,
                                                model::setLanguage,
                                                { model.signIn(context) },
                                                model::signOut,
                                            )
                                        }
                                        is Route.Game -> GameScreen(
                                            current.game,
                                            page,
                                            copy,
                                            haze,
                                            glass,
                                            model::back,
                                            { model.openAboutGame(current.game) },
                                        ) {
                                            model.signIn(context)
                                        }
                                        is Route.AboutGame -> AboutGameScreen(
                                            current.game,
                                            gameInfo,
                                            copy,
                                            haze,
                                            glass,
                                            model::back,
                                        )
                                    }
                                }
                            }
                            AnimatedVisibility(
                                visible = route is Route.Home && browser == null,
                                modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth(),
                                enter = fadeIn(tween(180)),
                                exit = fadeOut(tween(120)),
                            ) {
                                BottomBar(tab, copy, haze, glass, model::setTab)
                            }
                            UpdateModal(
                                update = update,
                                copy = copy,
                                haze = haze,
                                glass = glass,
                                onUpdate = { updates.startDownload(context) },
                                onLater = updates::later,
                            )
                            AnimatedVisibility(
                                visible = browser != null,
                                modifier = Modifier.fillMaxSize(),
                                enter = fadeIn(tween(220)),
                                exit = fadeOut(tween(160)),
                            ) {
                                browserPage?.let { SignInWeb(it, model::onBrowserCallback, model::cancelBrowser) }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun NexTheme(dark: Boolean, content: @Composable () -> Unit) {
    val scheme = if (dark) {
        darkColorScheme(
            primary = Color(0xFFFF453A),
            onPrimary = Color.White,
            background = Color(0xFF111113),
            surface = Color(0xFF1C1C1E),
            onSurface = Color(0xFFF5F5F7),
            secondary = Color(0xFFFF6B6B),
            tertiary = Color(0xFF64D2FF),
        )
    } else {
        lightColorScheme(
            primary = Color(0xFFE60012),
            onPrimary = Color.White,
            background = Color(0xFFF2F2F7),
            surface = Color.White,
            onSurface = Color(0xFF1C1C1E),
            secondary = Color(0xFFFF8A80),
            tertiary = Color(0xFF0A84FF),
        )
    }
    MaterialTheme(colorScheme = scheme, content = content)
}
