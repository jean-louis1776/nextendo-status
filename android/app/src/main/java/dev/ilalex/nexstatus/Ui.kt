package dev.ilalex.nexstatus

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.key
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.Velocity
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs
import kotlin.math.sign
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import androidx.compose.ui.viewinterop.AndroidView
import android.webkit.CookieManager
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import coil.compose.AsyncImage
import coil.request.ImageRequest
import dev.chrisbanes.haze.HazeInput
import dev.chrisbanes.haze.HazePerformanceMode
import dev.chrisbanes.haze.HazeState
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.glass.GlassStyle
import dev.chrisbanes.haze.glass.hazeGlass
import dev.chrisbanes.haze.glass.hazeGlassContainer

@Composable
fun Backdrop(modifier: Modifier = Modifier) {
    val scheme = MaterialTheme.colorScheme
    val shift by rememberInfiniteTransition(label = "drift").animateFloat(
        initialValue = 0f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(28_000, easing = LinearEasing), RepeatMode.Reverse),
        label = "shift",
    )
    Canvas(
        modifier.graphicsLayer {
            compositingStrategy = androidx.compose.ui.graphics.CompositingStrategy.Offscreen
        },
    ) {
        drawRect(scheme.background)
        fun blob(x: Float, y: Float, color: Color, radius: Float) {
            drawCircle(
                brush = Brush.radialGradient(listOf(color, Color.Transparent), Offset(x, y), radius),
                radius = radius,
                center = Offset(x, y),
            )
        }
        val dark = scheme.background.luminance() < 0.5f
        val punch = if (dark) 0.36f else 0.48f
        blob(size.width * (0.12f + shift * 0.14f), size.height * 0.08f, scheme.primary.copy(alpha = punch), size.minDimension * 0.7f)
        blob(size.width * (0.92f - shift * 0.08f), size.height * 0.32f, scheme.secondary.copy(alpha = punch * 0.8f), size.minDimension * 0.55f)
        blob(size.width * 0.4f, size.height * (0.95f - shift * 0.06f), scheme.tertiary.copy(alpha = punch * 0.75f), size.minDimension * 0.62f)
    }
}

@Composable
fun rememberGlass(dark: Boolean): GlassStyle = remember(dark) {
    GlassStyle.regular.then {
        shape(RoundedCornerShape(28.dp))
        if (dark) {
            tint(Color.Black.copy(alpha = 0.14f))
            backgroundColor(Color(0xFF161618).copy(alpha = 0.34f))
            edgeShadow(Color.Black.copy(alpha = 0.22f))
            specularIntensity(0.1f)
            ambientResponse(0.03f)
            contrast(-0.04f)
            whitePoint(-0.22f)
            chromaMultiplier(0.5f)
        } else {
            tint(Color.White.copy(alpha = 0.22f))
            backgroundColor(Color.White.copy(alpha = 0.26f))
            edgeShadow(Color(0xFF1C1C1E).copy(alpha = 0.08f))
        }
    }
}

@Composable
fun Modifier.glass(state: HazeState, style: GlassStyle, fast: Boolean = false): Modifier {
    val dark = MaterialTheme.colorScheme.background.luminance() < 0.5f
    val shape = RoundedCornerShape(28.dp)
    val edge = Brush.linearGradient(
        listOf(
            Color.White.copy(alpha = if (dark) 0.28f else 0.92f),
            Color.White.copy(alpha = if (dark) 0.05f else 0.22f),
        ),
    )
    return hazeGlass(
        input = HazeInput.Sources(state),
        style = style,
        performanceMode = HazePerformanceMode.Performance,
    ).border(1.dp, edge, shape)
}

@Composable
private fun Modifier.rubberBand(): Modifier {
    val pull = remember { mutableFloatStateOf(0f) }
    val scope = rememberCoroutineScope()
    val settle = remember { mutableStateOf<Job?>(null) }
    val connection = remember {
        object : NestedScrollConnection {
            override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
                if (source != NestedScrollSource.UserInput) return Offset.Zero
                settle.value?.cancel()
                val current = pull.floatValue
                val dy = available.y
                if (current == 0f || dy == 0f || sign(dy) == sign(current)) return Offset.Zero
                val next = current + dy
                return if (sign(next) != sign(current)) {
                    pull.floatValue = 0f
                    Offset(0f, -current)
                } else {
                    pull.floatValue = next
                    available
                }
            }

            override fun onPostScroll(consumed: Offset, available: Offset, source: NestedScrollSource): Offset {
                if (source != NestedScrollSource.UserInput || available.y == 0f) return Offset.Zero
                val limit = 140f
                val current = pull.floatValue
                val resistance = (1f - abs(current) / limit).coerceIn(0.15f, 1f)
                pull.floatValue = (current + available.y * 0.45f * resistance).coerceIn(-limit, limit)
                return available
            }

            override suspend fun onPreFling(available: Velocity): Velocity {
                val start = pull.floatValue
                if (start != 0f) {
                    settle.value = scope.launch {
                        animate(
                            start,
                            0f,
                            animationSpec = spring(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = Spring.StiffnessMediumLow),
                        ) { value, _ ->
                            pull.floatValue = value
                        }
                    }
                }
                return Velocity.Zero
            }
        }
    }
    return clipToBounds().nestedScroll(connection).graphicsLayer { translationY = pull.floatValue }
}

@Composable
private fun rememberTick(): () -> Unit {
    val view = LocalView.current
    return {
        view.isHapticFeedbackEnabled = true
        if (!view.performHapticFeedback(android.view.HapticFeedbackConstants.CONTEXT_CLICK)) {
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP)
        }
    }
}

@Composable
private fun Modifier.tap(onClick: () -> Unit): Modifier {
    val tick = rememberTick()
    return clip(RoundedCornerShape(28.dp)).clickable {
        tick()
        onClick()
    }
}

@Composable
private fun onlineColor(): Color {
    val dark = MaterialTheme.colorScheme.background.luminance() < 0.5f
    return if (dark) Color(0xFF8EE7B5) else Color(0xFF127A45)
}

@Composable
fun BottomBar(
    tab: MainTab,
    copy: Copy,
    haze: HazeState,
    glass: GlassStyle,
    onTab: (MainTab) -> Unit,
) {
    val bg = MaterialTheme.colorScheme.background
    Box(Modifier.fillMaxWidth()) {
        // Soft fade so game cards don't punch through the floating bar.
        Box(
            Modifier
                .matchParentSize()
                .background(
                    Brush.verticalGradient(
                        0f to bg.copy(alpha = 0f),
                        0.4f to bg.copy(alpha = 0.72f),
                        1f to bg.copy(alpha = 0.96f),
                    ),
                ),
        )
        Row(
            Modifier
                .fillMaxWidth()
                .navigationBarsPadding()
                .padding(horizontal = 16.dp, vertical = 10.dp)
                .glass(haze, glass)
                .background(bg.copy(alpha = 0.28f), RoundedCornerShape(28.dp))
                .padding(horizontal = 8.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            BottomTab(
                selected = tab == MainTab.Games,
                label = copy.gamesTab,
                modifier = Modifier.weight(1f),
                onClick = { onTab(MainTab.Games) },
            ) { GamepadIcon(Modifier.size(24.dp)) }
            BottomTab(
                selected = tab == MainTab.Account,
                label = copy.accountTab,
                modifier = Modifier.weight(1f),
                onClick = { onTab(MainTab.Account) },
            ) { PersonIcon(Modifier.size(24.dp)) }
        }
    }
}

@Composable
private fun BottomTab(
    selected: Boolean,
    label: String,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
    icon: @Composable () -> Unit,
) {
    val color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f)
    val tick = rememberTick()
    Column(
        modifier
            .clip(RoundedCornerShape(20.dp))
            .clickable {
                tick()
                onClick()
            }
            .padding(vertical = 8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        CompositionLocalProvider(LocalContentColor provides color) {
            icon()
            Text(label, color = color, style = MaterialTheme.typography.labelMedium, fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium)
        }
    }
}

@Composable
fun HomeScreen(
    home: HomeState,
    copy: Copy,
    haze: HazeState,
    glass: GlassStyle,
    onGame: (Game) -> Unit,
) {
    var shown by remember { mutableIntStateOf(16) }
    val games = home.games
    val gridState = rememberLazyGridState()
    Box(Modifier.fillMaxSize()) {
        LazyVerticalGrid(
            columns = GridCells.Adaptive(320.dp),
            state = gridState,
            modifier = Modifier.rubberBand().fillMaxSize().hazeGlassContainer(mergeDistance = 0.dp, scrollState = gridState),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 148.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item(span = { GridItemSpan(maxLineSpan) }) {
                Column(
                    Modifier.fillMaxWidth().statusBarsPadding().padding(top = 12.dp, bottom = 8.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        "NexStatus",
                        modifier = Modifier.fillMaxWidth(),
                        style = MaterialTheme.typography.displaySmall.copy(fontSize = 32.sp),
                        fontWeight = FontWeight.SemiBold,
                    )
                    if (games.isNotEmpty()) {
                        Spacer(Modifier.height(8.dp))
                        Text(
                            home.players.toString(),
                            style = MaterialTheme.typography.displayMedium,
                            fontWeight = FontWeight.SemiBold,
                        )
                        Text(
                            "${copy.playersWord(home.players)} ${copy.online}",
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.72f),
                        )
                    }
                    val subtitle = when {
                        home.error.isNotEmpty() && games.isEmpty() -> home.error
                        home.updated.isEmpty() -> ""
                        else -> {
                            val busy = games.count { it.players > 0 }
                            "${copy.gamesWithPlayers(busy, copy.gamesWord(busy))}  ·  ${home.updated}"
                        }
                    }
                    if (subtitle.isNotEmpty()) {
                        Text(
                            subtitle,
                            modifier = Modifier.padding(top = 12.dp),
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.66f),
                            style = MaterialTheme.typography.bodyMedium,
                            textAlign = TextAlign.Center,
                        )
                    }
                }
            }
            items(games.take(shown), key = { it.titleId.ifEmpty { it.name } }) { game ->
                GameCard(game, copy, haze, glass, fast = true) { onGame(game) }
            }
            if (shown < games.size) {
                item(span = { GridItemSpan(maxLineSpan) }) {
                    LaunchedEffect(shown) { shown += 12 }
                }
            }
        }
        if (home.loading && games.isEmpty()) {
            Column(
                Modifier.align(Alignment.Center),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                CircularProgressIndicator(color = MaterialTheme.colorScheme.primary)
                Text(copy.searchingServers, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.72f))
            }
        }
    }
}

@Composable
private fun GameCard(game: Game, copy: Copy, haze: HazeState, glass: GlassStyle, fast: Boolean, onClick: () -> Unit) {
    val live = onlineColor()
    Row(
        Modifier
            .fillMaxWidth()
            .glass(haze, glass, fast)
            .tap(onClick)
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        AsyncImage(
            model = iconRequest(game.titleId),
            contentDescription = null,
            modifier = Modifier.size(58.dp).clip(RoundedCornerShape(16.dp)),
            contentScale = ContentScale.Crop,
        )
        Column(Modifier.weight(1f).padding(horizontal = 12.dp)) {
            Text(game.name, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                if (game.players > 0) copy.online.replaceFirstChar { it.uppercase() } else copy.empty,
                color = if (game.players > 0) live else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                style = MaterialTheme.typography.bodyMedium,
            )
        }
        Column(horizontalAlignment = Alignment.End) {
            Text(game.players.toString(), style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
            Text(copy.playersWord(game.players), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f))
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun GameScreen(
    game: Game,
    page: GamePage?,
    copy: Copy,
    haze: HazeState,
    glass: GlassStyle,
    onBack: () -> Unit,
    onAbout: () -> Unit,
    onSignIn: () -> Unit,
) {
    val listState = rememberLazyListState()
    Box(Modifier.fillMaxSize()) {
        LazyColumn(
            state = listState,
            modifier = Modifier.rubberBand().fillMaxSize().navigationBarsPadding().hazeGlassContainer(mergeDistance = 0.dp, scrollState = listState),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                Row(Modifier.statusBarsPadding(), verticalAlignment = Alignment.CenterVertically) {
                    Box(
                        Modifier.glass(haze, glass).tap(onBack).padding(horizontal = 14.dp, vertical = 10.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        BackIcon(Modifier.size(18.dp))
                    }
                    Spacer(Modifier.width(12.dp))
                    AsyncImage(
                        model = iconRequest(game.titleId),
                        contentDescription = null,
                        modifier = Modifier.size(52.dp).clip(RoundedCornerShape(14.dp)),
                        contentScale = ContentScale.Crop,
                    )
                    Text(
                        game.name,
                        modifier = Modifier.weight(1f).padding(start = 12.dp),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            item {
                val aboutShape = RoundedCornerShape(28.dp)
                Text(
                    copy.aboutGame,
                    modifier = Modifier
                        .fillMaxWidth()
                        .glass(haze, glass)
                        .border(1.5.dp, Color(0xFFFF2D55), aboutShape)
                        .tap(onAbout)
                        .padding(horizontal = 18.dp, vertical = 14.dp),
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.Center,
                )
            }
            if (page != null) {
                if (page.note.isNotEmpty()) {
                    item {
                        Column(
                            Modifier.fillMaxWidth().glass(haze, glass).padding(18.dp),
                            verticalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            Text(page.note)
                            if (page.note == copy.signInToSee) {
                                Text(
                                    copy.signIn,
                                    modifier = Modifier.tap(onSignIn).padding(vertical = 4.dp),
                                    color = MaterialTheme.colorScheme.primary,
                                    fontWeight = FontWeight.SemiBold,
                                )
                            }
                        }
                    }
                }
                if (page.lobbies.isNotEmpty()) {
                    item { SectionTitle(copy.lobbies(page.lobbies.size)) }
                    items(page.lobbies.size) { index ->
                        LobbyCard(page.lobbies[index], haze, glass, fast = true)
                    }
                }
                if (page.players.isNotEmpty()) {
                    item { SectionTitle(copy.players(page.players.size)) }
                    item { People(page.players, haze, glass) }
                }
            }
        }
        if (page == null) {
            CircularProgressIndicator(Modifier.align(Alignment.Center), color = MaterialTheme.colorScheme.primary)
        }
    }
}

@Composable
fun AboutGameScreen(
    game: Game,
    info: GameInfo,
    copy: Copy,
    haze: HazeState,
    glass: GlassStyle,
    onBack: () -> Unit,
) {
    val listState = rememberLazyListState()
    Box(Modifier.fillMaxSize()) {
        LazyColumn(
            state = listState,
            modifier = Modifier.rubberBand().fillMaxSize().navigationBarsPadding().hazeGlassContainer(mergeDistance = 0.dp, scrollState = listState),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                Row(Modifier.statusBarsPadding(), verticalAlignment = Alignment.CenterVertically) {
                    Box(
                        Modifier.glass(haze, glass).tap(onBack).padding(horizontal = 14.dp, vertical = 10.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        BackIcon(Modifier.size(18.dp))
                    }
                    Spacer(Modifier.width(12.dp))
                    AsyncImage(
                        model = iconRequest(game.titleId),
                        contentDescription = null,
                        modifier = Modifier.size(52.dp).clip(RoundedCornerShape(14.dp)),
                        contentScale = ContentScale.Crop,
                    )
                    Column(Modifier.padding(start = 12.dp)) {
                        Text(copy.aboutGame, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.62f), style = MaterialTheme.typography.labelLarge)
                        Text(
                            info.title.ifEmpty { game.name },
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
            }
            if (info.loading) {
                item {
                    Row(
                        Modifier.fillMaxWidth().glass(haze, glass).padding(18.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp, color = MaterialTheme.colorScheme.primary)
                        Text(copy.loadingGameInfo)
                    }
                }
            } else if (info.error.isNotEmpty() && info.description.isEmpty() && info.screenshots.isEmpty()) {
                item { GlassNote(info.error, haze, glass) }
            } else {
                if (info.screenshots.isNotEmpty()) {
                    item { ScreenshotGallery(info.screenshots, haze, glass) }
                }
                if (info.intro.isNotEmpty()) {
                    item {
                        Text(
                            info.intro,
                            Modifier.fillMaxWidth().glass(haze, glass).padding(18.dp),
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                }
                if (info.description.isNotEmpty()) {
                    item {
                        Text(
                            info.description,
                            Modifier.fillMaxWidth().glass(haze, glass).padding(18.dp),
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.88f),
                        )
                    }
                } else if (info.screenshots.isEmpty() && info.intro.isEmpty()) {
                    item { GlassNote(copy.noDescription, haze, glass) }
                }
            }
        }
    }
}

@Composable
private fun ScreenshotGallery(shots: List<String>, haze: HazeState, glass: GlassStyle) {
    val pager = rememberPagerState { shots.size }
    Column(Modifier.fillMaxWidth().glass(haze, glass).padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        HorizontalPager(state = pager, pageSpacing = 10.dp) { page ->
            AsyncImage(
                model = shots[page],
                contentDescription = null,
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(16f / 9f)
                    .clip(RoundedCornerShape(18.dp))
                    .background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f)),
                contentScale = ContentScale.Crop,
            )
        }
        if (shots.size > 1) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center) {
                repeat(shots.size) { index ->
                    val on = pager.currentPage == index
                    Box(
                        Modifier
                            .padding(horizontal = 3.dp)
                            .size(if (on) 8.dp else 6.dp)
                            .clip(CircleShape)
                            .background(
                                if (on) MaterialTheme.colorScheme.primary
                                else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.28f),
                            ),
                    )
                }
            }
        }
    }
}

@Composable
fun AccountScreen(
    session: Session,
    history: HistoryState,
    copy: Copy,
    lang: LangChoice,
    haze: HazeState,
    glass: GlassStyle,
    onLanguage: (LangChoice) -> Unit,
    onSignIn: () -> Unit,
    onSignOut: () -> Unit,
    onAllGames: () -> Unit,
) {
    var copied by remember { mutableIntStateOf(0) }
    LaunchedEffect(copied) {
        val token = copied
        if (token == 0) return@LaunchedEffect
        delay(3000)
        if (copied == token) copied = 0
    }
    val clipboard = LocalClipboardManager.current
    val context = LocalContext.current
    val version = remember {
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName.orEmpty()
        }.getOrDefault("")
    }
    val listState = rememberLazyListState()
    LazyColumn(
        state = listState,
        modifier = Modifier.rubberBand().fillMaxSize().hazeGlassContainer(mergeDistance = 0.dp, scrollState = listState),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 148.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Text(
                copy.account,
                modifier = Modifier.fillMaxWidth().statusBarsPadding().padding(top = 12.dp, bottom = 4.dp),
                style = MaterialTheme.typography.displaySmall.copy(fontSize = 32.sp),
                fontWeight = FontWeight.SemiBold,
            )
        }
        item {
            if (session.signedIn) {
                Row(
                    Modifier.fillMaxWidth().glass(haze, glass).padding(16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Avatar(session, size = 64.dp)
                    Column(Modifier.weight(1f).padding(start = 14.dp)) {
                        Text(session.name.ifEmpty { copy.account }, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        if (session.friendCode.isNotEmpty()) {
                            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 4.dp)) {
                                Text(session.friendCode, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.78f))
                                Box(Modifier.padding(start = 8.dp).tap {
                                    clipboard.setText(AnnotatedString(session.friendCode))
                                    copied += 1
                                }.padding(4.dp)) {
                                    if (copied == 0) CopyIcon(Modifier.size(18.dp)) else CheckIcon(Modifier.size(18.dp))
                                }
                            }
                        }
                    }
                }
            } else if (session.status == copy.waitingBrowser) {
                Row(
                    Modifier.fillMaxWidth().glass(haze, glass).padding(18.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center,
                ) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp, color = MaterialTheme.colorScheme.primary)
                    Text(copy.waitingBrowser, modifier = Modifier.padding(start = 12.dp))
                }
            } else {
                Column(
                    Modifier.fillMaxWidth().glass(haze, glass).tap(onSignIn).padding(18.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Text(copy.signIn, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.primary)
                    if (session.status.isNotEmpty()) {
                        Text(
                            session.status,
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.66f),
                            style = MaterialTheme.typography.bodyMedium,
                            textAlign = TextAlign.Center,
                        )
                    }
                }
            }
        }
        if (session.signedIn) {
            item {
                Column(
                    Modifier.fillMaxWidth().glass(haze, glass).padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(copy.myGameHistory, fontWeight = FontWeight.SemiBold)
                    when {
                        history.loading && history.games.isEmpty() -> {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(12.dp),
                            ) {
                                CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp, color = MaterialTheme.colorScheme.primary)
                                Text(copy.loadingHistory, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.72f))
                            }
                        }
                        history.error.isNotEmpty() && history.games.isEmpty() -> {
                            Text(history.error, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.72f))
                        }
                        history.games.isEmpty() -> {
                            Text(copy.historyEmpty, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.72f))
                        }
                        else -> {
                            HistoryRow(history.games.first(), copy, haze, glass, fast = true)
                            Text(
                                copy.allMyGames,
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .tap(onAllGames)
                                    .padding(vertical = 10.dp),
                                color = MaterialTheme.colorScheme.primary,
                                fontWeight = FontWeight.SemiBold,
                                textAlign = TextAlign.Center,
                            )
                        }
                    }
                }
            }
        }
        item {
            Column(Modifier.fillMaxWidth().glass(haze, glass).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(copy.language, fontWeight = FontWeight.SemiBold)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    LangChip(copy.system, lang == LangChoice.System, haze, glass) { onLanguage(LangChoice.System) }
                    LangChip(copy.russian, lang == LangChoice.Russian, haze, glass) { onLanguage(LangChoice.Russian) }
                    LangChip(copy.english, lang == LangChoice.English, haze, glass) { onLanguage(LangChoice.English) }
                }
            }
        }
        item {
            Column(Modifier.fillMaxWidth().glass(haze, glass).padding(16.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                Text(copy.aboutApp, fontWeight = FontWeight.SemiBold)
                AboutRow(copy.application, "NexStatus")
                if (version.isNotEmpty()) AboutRow(copy.version, version.substringBefore(" · ").trim())
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(copy.author, modifier = Modifier.weight(1f), color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.62f))
                    Image(
                        painter = painterResource(R.drawable.author_ilalex),
                        contentDescription = null,
                        modifier = Modifier.size(28.dp).clip(CircleShape),
                        contentScale = ContentScale.Crop,
                    )
                    Text("ILALEX", modifier = Modifier.padding(start = 8.dp), fontWeight = FontWeight.SemiBold)
                }
            }
        }
        if (session.signedIn) {
            item {
                Text(
                    copy.signOut,
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(28.dp))
                        .background(MaterialTheme.colorScheme.primary)
                        .tap(onSignOut)
                        .padding(vertical = 14.dp),
                    color = MaterialTheme.colorScheme.onPrimary,
                    fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
fun HistoryScreen(
    history: HistoryState,
    copy: Copy,
    haze: HazeState,
    glass: GlassStyle,
    onBack: () -> Unit,
) {
    val listState = rememberLazyListState()
    Box(Modifier.fillMaxSize()) {
        LazyColumn(
            state = listState,
            modifier = Modifier.rubberBand().fillMaxSize().navigationBarsPadding().hazeGlassContainer(mergeDistance = 0.dp, scrollState = listState),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                Row(Modifier.statusBarsPadding(), verticalAlignment = Alignment.CenterVertically) {
                    Box(
                        Modifier.glass(haze, glass).tap(onBack).padding(horizontal = 14.dp, vertical = 10.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        BackIcon(Modifier.size(18.dp))
                    }
                    Text(
                        copy.gameHistory,
                        modifier = Modifier.padding(start = 12.dp),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
            when {
                history.loading && history.games.isEmpty() -> {
                    item {
                        Row(
                            Modifier.fillMaxWidth().glass(haze, glass).padding(18.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp, color = MaterialTheme.colorScheme.primary)
                            Text(copy.loadingHistory)
                        }
                    }
                }
                history.error.isNotEmpty() && history.games.isEmpty() -> {
                    item { GlassNote(history.error, haze, glass) }
                }
                history.games.isEmpty() -> {
                    item { GlassNote(copy.historyEmpty, haze, glass) }
                }
                else -> {
                    items(history.games.size, key = { history.games[it].titleId.ifEmpty { history.games[it].name } }) { index ->
                        HistoryRow(history.games[index], copy, haze, glass, fast = true)
                    }
                }
            }
        }
    }
}

@Composable
private fun HistoryRow(
    game: HistoryGame,
    copy: Copy,
    haze: HazeState,
    glass: GlassStyle,
    fast: Boolean,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .glass(haze, glass, fast)
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        AsyncImage(
            model = iconRequest(game.titleId),
            contentDescription = null,
            modifier = Modifier.size(58.dp).clip(RoundedCornerShape(16.dp)),
            contentScale = ContentScale.Crop,
        )
        Text(
            game.name,
            modifier = Modifier.weight(1f).padding(horizontal = 12.dp),
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            copy.playHours(game.seconds),
            color = MaterialTheme.colorScheme.primary,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

@Composable
private fun LangChip(label: String, selected: Boolean, haze: HazeState, glass: GlassStyle, onClick: () -> Unit) {
    val color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
    Text(
        label,
        modifier = Modifier
            .glass(haze, glass)
            .then(
                if (selected) Modifier.border(1.dp, MaterialTheme.colorScheme.primary, RoundedCornerShape(28.dp))
                else Modifier
            )
            .tap(onClick)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        color = color,
        fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
    )
}

@Composable
private fun AboutRow(label: String, value: String) {
    Row {
        Text(label, modifier = Modifier.weight(1f), color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.62f))
        Text(value, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(top = 6.dp))
}

@Composable
private fun GlassNote(text: String, haze: HazeState, glass: GlassStyle) {
    Text(text, Modifier.fillMaxWidth().glass(haze, glass).padding(18.dp))
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun LobbyCard(lobby: Lobby, haze: HazeState, glass: GlassStyle, fast: Boolean) {
    Column(Modifier.fillMaxWidth().glass(haze, glass, fast).padding(16.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(lobby.title, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            if (lobby.places.isNotEmpty()) Text(lobby.places, fontWeight = FontWeight.Medium)
        }
        val detail = listOf(lobby.mode, lobby.state).filter { it.isNotEmpty() }.joinToString("  ·  ")
        if (detail.isNotEmpty()) {
            Text(detail, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f), style = MaterialTheme.typography.bodyMedium)
        }
        if (lobby.members.isNotEmpty()) {
            Spacer(Modifier.height(10.dp))
            People(lobby.members, haze, glass)
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun People(people: List<Person>, haze: HazeState, glass: GlassStyle) {
    val friendTint = onlineColor()
    val chipShape = RoundedCornerShape(28.dp)
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        people.forEach { person ->
            val friend = person.mark.equals("Друг", ignoreCase = true) || person.mark.equals("Friend", ignoreCase = true)
            Row(
                Modifier
                    .glass(haze, glass)
                    .then(if (friend) Modifier.border(1.5.dp, friendTint, chipShape) else Modifier)
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(person.name, fontWeight = if (friend || person.mark.isNotEmpty()) FontWeight.SemiBold else FontWeight.Normal)
                if (person.mark.isNotEmpty()) {
                    Text(
                        person.mark,
                        modifier = Modifier.padding(start = 8.dp),
                        color = friendTint,
                        style = MaterialTheme.typography.labelLarge,
                    )
                }
                if (person.extra.isNotEmpty()) {
                    Text(
                        person.extra,
                        modifier = Modifier.padding(start = 8.dp),
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.62f),
                        style = MaterialTheme.typography.labelLarge,
                    )
                }
            }
        }
    }
}

@Composable
fun Stage(haze: HazeState, content: @Composable () -> Unit) {
    Box(Modifier.fillMaxSize()) {
        Backdrop(Modifier.hazeSource(haze).fillMaxSize())
        CompositionLocalProvider(LocalContentColor provides MaterialTheme.colorScheme.onSurface) {
            content()
        }
    }
}

@Composable
private fun Avatar(session: Session, size: Dp = 76.dp) {
    Box(Modifier.size(size).clip(CircleShape).background(MaterialTheme.colorScheme.primary.copy(alpha = 0.18f)), contentAlignment = Alignment.Center) {
        Text(
            session.name.take(1).uppercase().ifEmpty { "?" },
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.SemiBold,
        )
        if (session.avatar.isNotEmpty()) {
            AsyncImage(
                model = session.avatar,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        }
    }
}

@Composable
private fun BackIcon(modifier: Modifier = Modifier) {
    val color = LocalContentColor.current
    Canvas(modifier) {
        val w = size.minDimension
        val stroke = w * 0.16f
        drawLine(color, Offset(w * 0.62f, w * 0.18f), Offset(w * 0.28f, w * 0.5f), stroke, StrokeCap.Round)
        drawLine(color, Offset(w * 0.28f, w * 0.5f), Offset(w * 0.62f, w * 0.82f), stroke, StrokeCap.Round)
    }
}

@Composable
private fun GamepadIcon(modifier: Modifier = Modifier) {
    Icon(
        painter = painterResource(R.drawable.ic_gamepad),
        contentDescription = null,
        modifier = modifier,
        tint = LocalContentColor.current,
    )
}

@Composable
private fun PersonIcon(modifier: Modifier = Modifier) {
    val color = LocalContentColor.current
    Canvas(modifier) {
        val w = size.minDimension
        val stroke = w * 0.1f
        drawCircle(color, w * 0.18f, Offset(w * 0.5f, w * 0.32f), style = Stroke(stroke))
        val shoulders = Path().apply {
            moveTo(w * 0.18f, w * 0.86f)
            cubicTo(w * 0.18f, w * 0.62f, w * 0.82f, w * 0.62f, w * 0.82f, w * 0.86f)
        }
        drawPath(shoulders, color, style = Stroke(width = stroke, cap = StrokeCap.Round))
    }
}

@Composable
private fun CheckIcon(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val w = size.minDimension
        val stroke = w * 0.14f
        val color = Color(0xFF34C759)
        drawLine(color, Offset(w * 0.18f, w * 0.52f), Offset(w * 0.4f, w * 0.74f), stroke, StrokeCap.Round)
        drawLine(color, Offset(w * 0.4f, w * 0.74f), Offset(w * 0.82f, w * 0.28f), stroke, StrokeCap.Round)
    }
}

@Composable
private fun CopyIcon(modifier: Modifier = Modifier) {
    val color = LocalContentColor.current
    Canvas(modifier) {
        val w = size.minDimension
        val stroke = w * 0.1f
        val corner = CornerRadius(w * 0.12f, w * 0.12f)
        drawRoundRect(color, Offset(w * 0.28f, w * 0.06f), Size(w * 0.6f, w * 0.6f), corner, style = Stroke(stroke))
        drawRoundRect(color, Offset(w * 0.08f, w * 0.28f), Size(w * 0.6f, w * 0.6f), corner, style = Stroke(stroke))
    }
}

@Composable
fun UpdateModal(
    update: AppUpdate,
    copy: Copy,
    haze: HazeState,
    glass: GlassStyle,
    onUpdate: () -> Unit,
    onLater: () -> Unit,
) {
    val visible = update !is AppUpdate.Idle
    AnimatedVisibility(
        visible = visible,
        modifier = Modifier.fillMaxSize(),
        enter = fadeIn(tween(200)) + scaleIn(initialScale = 0.96f, animationSpec = tween(200)),
        exit = fadeOut(tween(140)) + scaleOut(targetScale = 0.98f, animationSpec = tween(140)),
    ) {
        val quiet = remember { MutableInteractionSource() }
        val version = when (update) {
            is AppUpdate.Available -> update.version
            is AppUpdate.Downloading -> update.version
            is AppUpdate.Failed -> update.version
            AppUpdate.Idle -> ""
        }
        val downloading = update is AppUpdate.Downloading
        val onSurface = MaterialTheme.colorScheme.onSurface
        CompositionLocalProvider(LocalContentColor provides onSurface) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.28f))
                    .then(
                        if (downloading) Modifier
                        else Modifier.clickable(quiet, indication = null, onClick = onLater),
                    ),
            ) {
                Column(
                    Modifier
                        .align(Alignment.Center)
                        .padding(horizontal = 18.dp)
                        .fillMaxWidth()
                        .glass(haze, glass)
                        .clickable(remember { MutableInteractionSource() }, indication = null, onClick = {})
                        .padding(22.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(
                        copy.updateTitle,
                        color = onSurface,
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(copy.updateBody(version), color = onSurface.copy(alpha = 0.78f))
                    when (update) {
                        is AppUpdate.Downloading -> {
                            Text(copy.updateDownloading, color = onSurface, fontWeight = FontWeight.Medium)
                            UpdateBar(progress = if (update.total > 0) update.got.toFloat() / update.total.toFloat() else 0f)
                            Text(
                                downloadStats(update.got, update.total, update.speed),
                                color = onSurface.copy(alpha = 0.72f),
                                style = MaterialTheme.typography.bodyMedium,
                            )
                        }
                        is AppUpdate.Failed -> {
                            Text(copy.updateFailed, color = MaterialTheme.colorScheme.primary)
                            Row(
                                Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(28.dp, Alignment.End),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(
                                    copy.updateLater,
                                    modifier = Modifier.tap(onLater).padding(vertical = 4.dp),
                                    color = onSurface.copy(alpha = 0.78f),
                                    fontWeight = FontWeight.Medium,
                                )
                                Text(
                                    copy.updateRetry,
                                    modifier = Modifier.tap(onUpdate).padding(vertical = 4.dp),
                                    color = MaterialTheme.colorScheme.primary,
                                    fontWeight = FontWeight.SemiBold,
                                )
                            }
                        }
                        is AppUpdate.Available -> {
                            Row(
                                Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(28.dp, Alignment.End),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(
                                    copy.updateLater,
                                    modifier = Modifier.tap(onLater).padding(vertical = 4.dp),
                                    color = onSurface.copy(alpha = 0.78f),
                                    fontWeight = FontWeight.Medium,
                                )
                                Text(
                                    copy.updateNow,
                                    modifier = Modifier.tap(onUpdate).padding(vertical = 4.dp),
                                    color = MaterialTheme.colorScheme.primary,
                                    fontWeight = FontWeight.SemiBold,
                                )
                            }
                        }
                        AppUpdate.Idle -> Unit
                    }
                }
            }
        }
    }
}

@Composable
private fun UpdateBar(progress: Float) {
    val shape = RoundedCornerShape(percent = 50)
    Box(
        Modifier
            .fillMaxWidth()
            .height(10.dp)
            .clip(shape)
            .background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f)),
    ) {
        Box(
            Modifier
                .fillMaxWidth(progress.coerceIn(0f, 1f))
                .height(10.dp)
                .clip(shape)
                .background(Color(0xFFFF2D55)),
        )
    }
}

@Composable
fun SignInWeb(url: String, copy: Copy, onResult: (String) -> Unit, onClose: () -> Unit) {
    Box(Modifier.fillMaxSize().background(Color(0xFF111113))) {
        key(url) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { context ->
                    WebView(context).apply {
                        layoutParams = android.view.ViewGroup.LayoutParams(
                            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                            android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                        )
                        setBackgroundColor(0xFF111113.toInt())
                        settings.javaScriptEnabled = true
                        settings.domStorageEnabled = true
                        settings.useWideViewPort = true
                        settings.loadWithOverviewMode = true
                        CookieManager.getInstance().setAcceptCookie(true)
                        CookieManager.getInstance().setAcceptThirdPartyCookies(this, true)
                        webViewClient = object : WebViewClient() {
                            private var handed = false
                            private fun take(target: String?): Boolean {
                                if (target == null || !target.contains("/callback")) return false
                                if (!handed) {
                                    handed = true
                                    onResult(target)
                                }
                                return true
                            }
                            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                                return take(request.url.toString())
                            }
                            override fun onPageStarted(view: WebView, started: String?, favicon: android.graphics.Bitmap?) {
                                if (take(started)) view.stopLoading()
                            }
                        }
                        fun openWhenReady() {
                            if (getUrl().isNullOrEmpty() && height > 200) loadUrl(url)
                        }
                        addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> openWhenReady() }
                        post { openWhenReady() }
                    }
                },
            )
        }
        Text(
            copy.close,
            modifier = Modifier
                .align(Alignment.TopStart)
                .statusBarsPadding()
                .padding(16.dp)
                .background(Color(0xFF111113).copy(alpha = 0.86f), RoundedCornerShape(12.dp))
                .tap(onClose)
                .padding(horizontal = 14.dp, vertical = 8.dp),
            color = Color.White,
            fontWeight = FontWeight.Medium,
        )
    }
}

@Composable
private fun iconRequest(titleId: String): ImageRequest {
    val context = LocalContext.current
    return remember(titleId) {
        ImageRequest.Builder(context)
            .data("https://nextendo.network/api/game-icon/$titleId.jpg")
            .size(180)
            .crossfade(false)
            .build()
    }
}
