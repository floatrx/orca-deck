import ApexCore
import SwiftUI
import WatchKit

/// Page 0 — the knob, adapted to the watch's rounded-rectangle display: one dot per worktree rides a track
/// that follows the screen's shape, the chosen dot always at top centre. The crown moves the dots one
/// worktree per detent (one haptic tick each, wrapping round); the centre shows the chosen worktree like the
/// mock (bridge/index.html). Tap or Double Tap focuses it in Orca; a long press shows the worktree's next
/// agent. It is alive: the background glows in the agent's colour, a comet runs the track while the agent
/// works, sonar rings leave the chosen dot while it needs you, and the centre slides with the crown.
struct KnobView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var keepWatching: KeepWatching
    /// This page is on screen — only then does the knob own the crown and animate.
    let active: Bool

    @Environment(\.isLuminanceReduced) private var dimmed
    @State private var crown = 0.0
    /// The track's offset in turns: −selected/count, reached the short way round.
    @State private var phase = 0.0
    /// A crown step slides the track from `phaseFrom` to `phase`, starting at `slideStart` (Dial.slide) —
    /// computed per frame, so the dots move along the track only.
    @State private var phaseFrom = 0.0
    @State private var slideStart = Date.distantPast
    /// A crown step is sliding: only then does the dots' timeline run, at the display's full rate.
    @State private var sliding = false
    @State private var slideToken = 0
    /// The dot chosen before the current one: it shrinks back while the new one grows in.
    @State private var previousSelected: Int?
    /// Measures the frame gaps of the first crown steps after launch and reports them to the trail.
    @State private var frameProbe = FrameProbe()
    /// +1 when the crown moved forward, −1 backward — the centre slides in from that side.
    @State private var direction = 1
    /// A long press already acted; the button's tap on release must not focus as well.
    @State private var longPressed = false
    @FocusState private var focused: Bool

    /// Until the display's concentric radii are measured, and before watchOS 27: what the Ultra 3 measures
    /// for the 7 pt inset (both the simulator and the real watch report 57).
    private static let fallbackRadius: CGFloat = 57
    /// The track's corner radii, concentric with the display's corners (ConcentricRadius). The system clock
    /// is hidden, so the top corners hug the display as closely as the bottom ones.
    @State private var topRadius = KnobView.fallbackRadius
    @State private var bottomRadius = KnobView.fallbackRadius
    @State private var reportedRadius = false
    @State private var reportedTransient = false
    /// The knob's canvas, for the radius report: the real watch and the simulator must agree.
    @State private var canvas = CGSize.zero
    /// Seconds the animations spent paused (Always-On, page off screen), taken off their clock: on the wrist
    /// raise the comet resumes where it stopped instead of jumping across the track.
    @State private var pausedTotal: TimeInterval = 0
    @State private var pausedAt: Date?
    private var paused: Bool { dimmed || !active }

    private var count: Int { model.worktrees.count }
    /// What the dots' Canvas draws per worktree.
    private var dotModels: [PerimeterDots.Dot] {
        model.worktrees.map { PerimeterDots.Dot(bucket: Agents.shown($0, picked: model.picked[$0.id])?.bucket,
                                                empty: $0.agents.isEmpty) }
    }
    /// A dot needs you: the dots' timeline keeps running at 15 fps for its pulse.
    private var dotsBreathe: Bool { dotModels.contains { !$0.empty && $0.bucket == .attention } }
    private var bucket: Bucket? { model.shownAgent?.bucket }
    /// The bucket the background, the halo, the comet and the sonar show: the shown agent's, once the crown has
    /// rested for `restDelay`. Turning past five worktrees must not flash five colours across the screen.
    @State private var restingBucket: Bucket?
    @State private var restToken = 0
    private static let restDelay: UInt64 = 350_000_000

    var body: some View {
        GeometryReader { geometry in
            let track = Self.trackRect(in: geometry.size)
            let topRadius = topRadius + DebugLaunch.cornerOffset
            let bottomRadius = bottomRadius + DebugLaunch.cornerOffset
            ZStack {
                // The glow drifts slowly: 15 frames a second are plenty and leave the GPU to the crown's slide.
                // Paused during a crown step's slide: the GPU goes to the dots for those 0.38 s.
                TimelineView(.animation(minimumInterval: 1 / 15, paused: paused || sliding)) { context in
                    Aurora(bucket: restingBucket, time: context.date.timeIntervalSinceReferenceDate - pausedTotal)
                        .id(restingBucket)
                        .transition(.opacity)
                }
                .animation(.easeInOut(duration: 0.6), value: restingBucket)
                if restingBucket == .working || restingBucket == .attention {
                    TimelineView(.animation(minimumInterval: 1 / 30, paused: paused)) { context in
                        let time = context.date.timeIntervalSinceReferenceDate - pausedTotal
                        if restingBucket == .working {
                            Comet(rect: track, top: topRadius, bottom: bottomRadius, time: time)
                        } else {
                            Sonar(center: Track.point(at: 0, in: track, top: topRadius, bottom: bottomRadius), time: time)
                        }
                    }
                }
                // Measures the track's frame against the display: its corners become concentric.
                Color.clear
                    .frame(width: track.width, height: track.height)
                    .position(x: track.midX, y: track.midY)
                    .modifier(ConcentricRadius(edge: .top, fallback: Self.fallbackRadius, radius: $topRadius,
                                               onTransient: reportTransient))
                    .modifier(ConcentricRadius(edge: .bottom, fallback: Self.fallbackRadius, radius: $bottomRadius,
                                               onTransient: reportTransient))
                TrackHalo(bucket: restingBucket, rect: track, top: topRadius, bottom: bottomRadius)
                // The dots move only on a crown step: their timeline runs at the display's full rate for the
                // slide (Dial.slideSettle) and is paused the rest of the time.
                // Full rate while a step slides; 15 fps while a dot breathes for attention; paused otherwise.
                TimelineView(.animation(minimumInterval: sliding ? nil : 1 / 15,
                                        paused: paused || !(sliding || dotsBreathe))) { context in
                    PerimeterDots(dots: dotModels, selected: model.selected, previous: previousSelected,
                                  selectedAt: slideStart, rect: track, top: topRadius, bottom: bottomRadius,
                                  phase: sliding && !paused ? frameProbe.frame(dotsPhase(at: context.date), at: context.date) : phase,
                                  date: context.date)
                }
                centre
                    .frame(width: track.width - 26, height: track.height - 40)
                    .position(x: track.midX, y: track.midY)
                // Swipe hints in the bottom corners: Settings on the left, the agent on the right.
                Image(systemName: "gearshape.fill")
                    .font(.system(size: 10))
                    .foregroundStyle(Theme.muted.opacity(0.5))
                    .position(Self.hintPoint(in: track, radius: bottomRadius, leading: true))
                    .accessibilityHidden(true)
                Image(systemName: "chevron.compact.right")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(Theme.muted.opacity(0.6))
                    .position(Self.hintPoint(in: track, radius: bottomRadius, leading: false))
                    .accessibilityHidden(true)
                // For UI tests: the crown's detent count, so a test checks "one detent = one worktree" against the
                // detents the crown really produced (the simulator's turn-to-detent conversion varies).
                Color.clear
                    .frame(width: 1, height: 1)
                    .accessibilityElement()
                    .accessibilityIdentifier("knob-crown")
                    .accessibilityValue("\(Int(crown.rounded()))")
            }
            // Scoped to the shake: an event must not animate anything else in its transaction (the comet and
            // the dots would cut straight across the screen instead of following the track).
            .animation(.linear(duration: 0.45)) { $0.modifier(Shake(travel: CGFloat(model.pulse))) }
            .onAppear { canvas = geometry.size }
            .onChange(of: geometry.size) { _, size in canvas = size }
        }
        .ignoresSafeArea()
        .focusable(active)
        .focused($focused)
        // An unbounded crown range: every detent is one worktree and the wrap is modulo arithmetic, so the
        // seam never costs a dead tick (Dial.index(forCrown:) / Dial.crownValue(for:near:)).
        .digitalCrownRotation(detent: $crown,
                              from: -100_000,
                              through: 100_000,
                              by: 1,
                              sensitivity: .low,
                              isContinuous: false,
                              isHapticFeedbackEnabled: true)
        .onChange(of: crown) { old, value in
            guard count > 0 else { return }
            direction = value >= old ? 1 : -1
            model.select(Dial.index(forCrown: value, count: count))
        }
        .onChange(of: model.selected) { old, index in
            previousSelected = old
            aim(at: index, animated: true)
        }
        .onChange(of: bucket) { _, _ in settleBucket() }
        .onChange(of: count) { _, _ in aim(at: model.selected, animated: false) }
        .onChange(of: active) { _, isActive in focused = isActive }
        .onChange(of: paused) { _, isPaused in
            if isPaused {
                pausedAt = Date()
            } else if let since = pausedAt {
                pausedTotal += Date().timeIntervalSince(since)
                pausedAt = nil
            }
        }
        .task {
            // Once per launch, after the launch settles: the Mac's trail shows the radii the track draws with.
            guard !reportedRadius else { return }
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            reportedRadius = true
            let screen = WKInterfaceDevice.current().screenBounds.size
            model.report("knob: corner radii top \(Int(topRadius)) / bottom \(Int(bottomRadius)) pt · canvas \(Int(canvas.width))×\(Int(canvas.height)) · screen \(Int(screen.width))×\(Int(screen.height))")
        }
        .onAppear {
            focused = active
            restingBucket = bucket
            aim(at: model.selected, animated: false)
        }
    }

    /// Once per launch: a measurement the track ignored (ConcentricRadius), so the trail shows it happened.
    private func reportTransient(_ measured: CGFloat) {
        guard !reportedTransient else { return }
        reportedTransient = true
        model.report("knob: ignored a transient corner radius of \(Int(measured)) pt")
    }

    /// The track: the whole display, inset 7 pt on every side — the app hides the system clock (ContentView).
    static func trackRect(in size: CGSize) -> CGRect {
        CGRect(x: 7, y: 7, width: size.width - 14, height: size.height - 14)
    }

    /// A swipe hint's place: on the bottom corner's diagonal, `inset` inside the track's arc — concentric with
    /// it, so the hint never crowds the line (at 57 pt: 31 pt in from the side and the bottom).
    static func hintPoint(in track: CGRect, radius: CGFloat, leading: Bool, inset: CGFloat = 20) -> CGPoint {
        let reach = max(radius - inset, 0) / 2.squareRoot()
        let centreX = leading ? track.minX + radius : track.maxX - radius
        return CGPoint(x: centreX + (leading ? -reach : reach), y: track.maxY - radius + reach)
    }

    /// Moves the dots so `index` sits at top centre, the short way round, and keeps the crown's value on it.
    private func aim(at index: Int, animated: Bool) {
        if count > 0, Dial.index(forCrown: crown, count: count) != index {
            crown = Dial.crownValue(for: index, near: crown, count: count)
        }
        let target = Dial.rotation(selected: index, count: count, current: phase * 360) / 360
        if animated {
            // From wherever the dots are now, so a quick second step continues the slide without a jump.
            let now = Date()
            phaseFrom = sliding ? dotsPhase(at: now) : phase
            slideStart = now
            startSlide()
        } else {
            phaseFrom = target
        }
        phase = target
    }

    /// Shows the shown agent's bucket in the background once the crown has rested; a newer change restarts it.
    private func settleBucket() {
        restToken += 1
        let token = restToken
        Task {
            try? await Task.sleep(nanoseconds: Self.restDelay)
            guard token == restToken, restingBucket != bucket else { return }
            restingBucket = bucket
        }
    }

    /// Runs the dots' timeline for one slide; a newer step extends it. The first slides report their frame gaps.
    private func startSlide() {
        slideToken += 1
        let token = slideToken
        if !sliding { frameProbe.begin() }
        sliding = true
        Task {
            try? await Task.sleep(nanoseconds: UInt64((Dial.slideSettle + 0.05) * 1_000_000_000))
            guard token == slideToken else { return }
            sliding = false
            if let line = frameProbe.finish() { model.report(line) }
        }
    }

    /// The track's offset at `date`: on the way from `phaseFrom` to `phase` while a crown step slides.
    private func dotsPhase(at date: Date) -> Double {
        phaseFrom + (phase - phaseFrom) * Dial.slide(after: date.timeIntervalSince(slideStart))
    }

    private var centre: some View {
        Button {
            if longPressed {
                longPressed = false
            } else {
                model.focusSelected()
            }
        } label: {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                CentreStack(model: model,
                            keepWatching: keepWatching.running,
                            direction: direction,
                            nowMs: context.date.timeIntervalSince1970 * 1000)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .simultaneousGesture(LongPressGesture(minimumDuration: 0.45).onEnded { _ in
            longPressed = true
            model.pickNextAgent()
            // If the finger slides off, the button never fires: do not let the flag swallow a later tap.
            Task {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                longPressed = false
            }
        })
        .modifier(DoubleTap(enabled: active))
        .accessibilityIdentifier("knob-centre")
        .accessibilityValue(count > 0 ? "\(model.selected + 1)/\(count)" : "0/0")
        .accessibilityLabel(model.selectedWorktree.map { "\(Centre.title($0)), \(Centre.state(model.shownAgent))" } ?? "No worktree")
        .accessibilityHint("Double tap to show in Orca. Swipe left for the agent, right for Settings.")
    }
}

/// The mock's centre, laid out for the full width: summary · state chip · name · meta · detail · counter.
/// A new worktree slides in from the side the crown turned to.
private struct CentreStack: View {
    @ObservedObject var model: AppModel
    let keepWatching: Bool
    let direction: Int
    let nowMs: Double

    /// The worktree whose card is on screen. A crown step fades the card out, then swaps it, then fades it in —
    /// one card at a time, never two texts on top of each other (user, 2026-10-26: the overlapping transition
    /// read as lag).
    @State private var shownId: String?
    @State private var cardOpacity = 1.0
    @State private var cardOffset: CGFloat = 0
    @State private var swapping = false

    var body: some View {
        VStack(spacing: 4) {
            HStack(spacing: 3) {
                if keepWatching {
                    Image(systemName: "eye.fill").font(.system(size: 8)).foregroundStyle(Theme.muted)
                }
                Text(summary)
                    .font(.system(size: 9, weight: .semibold, design: .monospaced))
                    .kerning(0.6)
                    .textCase(.uppercase)
                    .foregroundStyle(summaryColor)
                    .lineLimit(1)
                    .contentTransition(.opacity)
            }
            .animation(.easeInOut(duration: 0.25), value: summary)
            Spacer(minLength: 0)
            ZStack {
                if let worktree = model.worktrees.first(where: { $0.id == shownId }) ?? model.selectedWorktree {
                    WorktreeCard(model: model, worktree: worktree, nowMs: nowMs)
                        .opacity(cardOpacity)
                        .offset(y: cardOffset)
                } else {
                    Text(model.connection != .online ? "waiting for the bridge…"
                         : model.hiddenCount > 0 ? "nothing active" : "no workspaces")
                        .font(.system(size: 12))
                        .foregroundStyle(Theme.muted)
                }
            }
            .onAppear { shownId = model.selectedWorktree?.id }
            .onChange(of: model.selectedWorktree?.id) { _, _ in swapCard() }
            Spacer(minLength: 0)
        }
    }

    /// Out in 70 ms, swap to the selection as it is then (several quick steps collapse into one swap), in over
    /// 160 ms with a 4 pt drift from the crown's direction.
    private func swapCard() {
        guard !swapping else { return }
        guard model.selectedWorktree?.id != shownId else { return }
        swapping = true
        withAnimation(.easeIn(duration: 0.07)) {
            cardOpacity = 0
        } completion: {
            shownId = model.selectedWorktree?.id
            cardOffset = direction > 0 ? 4 : -4
            withAnimation(.easeOut(duration: 0.16)) {
                cardOpacity = 1
                cardOffset = 0
            }
            swapping = false
            // A step that landed during the fade-out still needs its card.
            if model.selectedWorktree?.id != shownId { swapCard() }
        }
    }

    /// The last event or failure while it shows, else the connection when it is not online, else the
    /// mock's agent summary.
    private var summary: String {
        if let banner = model.banner { return banner }
        if model.connection != .online { return model.connectionLabel }
        return Centre.summary(model.allWorktrees)
    }

    private var summaryColor: Color {
        if model.banner != nil { return Theme.attention }
        switch model.connection {
        case .online, .connecting: return Theme.muted
        case .offline, .outdated: return .red
        }
    }
}

/// The chosen worktree: state chip, name, meta, detail and the rolling counter.
private struct WorktreeCard: View {
    @ObservedObject var model: AppModel
    let worktree: Worktree
    let nowMs: Double

    var body: some View {
        // Everything from this card's own worktree, not the current selection: while the card fades out after a
        // crown step, its texts must not jump to the next worktree.
        let agent = Agents.shown(worktree, picked: model.picked[worktree.id])
        let index = model.worktrees.firstIndex(where: { $0.id == worktree.id }) ?? model.selected
        let counter = Centre.counter(index: index, count: model.worktrees.count,
                                     worktree: worktree, agent: agent, nowMs: nowMs)
        VStack(spacing: 4) {
            StateChip(agent: agent)
            Text(Centre.title(worktree))
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(Theme.ink)
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .minimumScaleFactor(0.7)
            let meta = Centre.meta(worktree, agent: agent)
            if !meta.isEmpty {
                Text(meta)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(Theme.muted)
                    .lineLimit(1)
            }
            let detail = Centre.detail(agent)
            if !detail.isEmpty {
                Text(detail)
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.detail)
                    .multilineTextAlignment(.center)
                    // Without the clock the centre has room for more of the agent's last message.
                    .lineLimit(6)
                    .padding(.top, 1)
                    .contentTransition(.opacity)
                    .animation(.easeInOut(duration: 0.3), value: detail)
            }
            if model.pending == worktree.id {
                ProgressView().frame(width: 14, height: 14).padding(.top, 4)
            } else {
                Text(counter)
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .monospacedDigit()
                    .foregroundStyle(Theme.muted)
                    .lineLimit(1)
                    .contentTransition(.numericText())
                    .animation(.snappy, value: counter)
                    .padding(.top, 4)
            }
        }
    }
}

/// The agent's state as a tinted capsule; its dot breathes while the agent works or needs you.
private struct StateChip: View {
    let agent: Agent?

    var body: some View {
        let color = Theme.color(agent?.bucket)
        let busy = agent?.bucket == .working || agent?.bucket == .attention
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 5, height: 5)
                .modifier(Pulse(enabled: busy))
            Text(Centre.state(agent))
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .kerning(1)
                .foregroundStyle(color)
                .contentTransition(.opacity)
        }
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .background(Capsule().fill(color.opacity(0.16)))
        .animation(.easeInOut(duration: 0.3), value: agent?.bucket)
    }
}

/// The track as a line, glowing in the shown agent's colour; it breathes while that agent needs you.
private struct TrackHalo: View {
    let bucket: Bucket?
    let rect: CGRect
    let top: CGFloat
    let bottom: CGFloat

    var body: some View {
        // Circular corners, the same arcs Track.point walks — the dots sit exactly on the line.
        let shape = UnevenRoundedRectangle(topLeadingRadius: top, bottomLeadingRadius: bottom,
                                           bottomTrailingRadius: bottom, topTrailingRadius: top, style: .circular)
        ZStack {
            shape
                .stroke(Color.white.opacity(0.07), lineWidth: 1)
            shape
                .stroke(Theme.color(bucket), lineWidth: 2)
                .opacity(0.3)
                .modifier(Pulse(enabled: bucket == .attention))
                .animation(.easeInOut(duration: 0.5), value: bucket)
        }
        .frame(width: rect.width, height: rect.height)
        .position(x: rect.midX, y: rect.midY)
    }
}

/// While the agent works: a comet with a fading tail runs clockwise along the track. Drawn in one Canvas — 17
/// SwiftUI views redrawn 30 times a second cost the watch frames (measured 2026-10-26). Its position comes from
/// `time` alone: it moves only along the track.
private struct Comet: View {
    let rect: CGRect
    let top: CGFloat
    let bottom: CGFloat
    let time: Double

    private static let lap: Double = 2.8
    private static let tail = 16

    var body: some View {
        let head = (time / Self.lap).truncatingRemainder(dividingBy: 1)
        Canvas { context, _ in
            for step in stride(from: Self.tail - 1, through: 0, by: -1) {
                let fade = 1 - Double(step) / Double(Self.tail)
                let point = Track.point(at: head - Double(step) * 0.007, in: rect, top: top, bottom: bottom)
                context.fill(Self.circle(at: point, diameter: 2 + 3 * fade), with: .color(Theme.working.opacity(0.9 * fade)))
            }
            let point = Track.point(at: head, in: rect, top: top, bottom: bottom)
            context.fill(Self.circle(at: point, diameter: 11), with: .color(Theme.working.opacity(0.45)))
            context.fill(Self.circle(at: point, diameter: 4), with: .color(.white))
        }
        .allowsHitTesting(false)
    }

    static func circle(at point: CGPoint, diameter: CGFloat) -> Path {
        Path(ellipseIn: CGRect(x: point.x - diameter / 2, y: point.y - diameter / 2, width: diameter, height: diameter))
    }
}

/// While the agent needs you: sonar rings leave the chosen dot — the same signal as the app icon.
private struct Sonar: View {
    let center: CGPoint
    let time: Double

    private static let period: Double = 1.8

    var body: some View {
        ZStack {
            ForEach(0..<2, id: \.self) { ring in
                let progress = (time / Self.period + Double(ring) * 0.5).truncatingRemainder(dividingBy: 1)
                Circle()
                    .stroke(Theme.attention, lineWidth: 1.5)
                    .frame(width: 12 + 60 * progress, height: 12 + 60 * progress)
                    .opacity(0.7 * (1 - progress))
                    .position(center)
            }
        }
        .transaction { $0.animation = nil }
        .allowsHitTesting(false)
    }
}

/// One dot per worktree on the track at `phase` (turns), drawn in one Canvas: a crown step redraws the dots on
/// every frame, and one SwiftUI view per dot (each with its own animations) held the real watch at ~20 fps
/// (FrameProbe, 2026-10-26). Everything that moves is computed from time — the slide's phase, the chosen dot
/// growing in (`selectedAt`), the attention pulse — so no implicit animation can ever cut across the screen.
private struct PerimeterDots: View {
    struct Dot {
        let bucket: Bucket?
        let empty: Bool
    }

    let dots: [Dot]
    let selected: Int
    /// The dot that was chosen before `selected`, shrinking back while the new one grows.
    let previous: Int?
    let selectedAt: Date
    let rect: CGRect
    let top: CGFloat
    let bottom: CGFloat
    let phase: Double
    let date: Date

    private static let size: CGFloat = 12
    private static let small: CGFloat = 0.58
    private static let grow: Double = 0.2

    var body: some View {
        Canvas { context, _ in
            let count = max(dots.count, 1)
            let grown = min(1, max(0, date.timeIntervalSince(selectedAt) / Self.grow))
            let eased = 1 - (1 - grown) * (1 - grown)
            // The attention pulse: opacity 1 → 0.4 → 1 over 1.1 s, like the Pulse modifier it replaces.
            let pulse = 0.7 + 0.3 * cos(date.timeIntervalSinceReferenceDate * 2 * .pi / 1.1)
            for (index, dot) in dots.enumerated() {
                let point = Track.point(at: Double(index) / Double(count) + phase, in: rect, top: top, bottom: bottom)
                let scale: CGFloat
                if index == selected {
                    scale = Self.small + (1 - Self.small) * eased
                } else if index == previous {
                    scale = 1 - (1 - Self.small) * eased
                } else {
                    scale = Self.small
                }
                let diameter = Self.size * scale
                let isSelected = index == selected
                if dot.empty {
                    let ring = Comet.circle(at: point, diameter: diameter - (isSelected ? 1.5 : 1))
                    context.fill(Comet.circle(at: point, diameter: diameter), with: .color(.black))
                    context.stroke(ring, with: .color(isSelected ? Theme.ink : Theme.idle), lineWidth: isSelected ? 1.5 : 1)
                } else {
                    let color = Theme.color(dot.bucket)
                    let breathing = dot.bucket == .attention ? pulse : 1
                    if isSelected {
                        context.fill(Comet.circle(at: point, diameter: diameter * 1.9), with: .color(color.opacity(0.3 * eased)))
                    }
                    context.fill(Comet.circle(at: point, diameter: diameter),
                                 with: .color(color.opacity((isSelected ? 1 : 0.7) * breathing)))
                }
            }
        }
        .allowsHitTesting(false)
    }
}

/// Frame gaps of a crown step's slide, as the dots' timeline delivers them: the first five slides after launch
/// go to the trail ("knob slide: 36 frames · avg 16.7 ms · worst 25 ms"), so a lag on the real watch is measured,
/// not guessed. A reference type: recording a frame must not invalidate the view.
final class FrameProbe {
    private var dates: [Date] = []
    private var reported = 0

    func begin() { dates.removeAll(keepingCapacity: true) }

    /// Records the frame at `date` and passes `value` through, so it can sit inside the view's expression.
    func frame(_ value: Double, at date: Date) -> Double {
        if dates.last != date { dates.append(date) }
        return value
    }

    func finish() -> String? {
        guard reported < 5, dates.count > 2 else { return nil }
        reported += 1
        let gaps = zip(dates.dropFirst(), dates).map { $0.timeIntervalSince($1) * 1000 }
        let average = gaps.reduce(0, +) / Double(gaps.count)
        return String(format: "knob slide: %d frames · avg %.1f ms · worst %.0f ms", dates.count, average, gaps.max() ?? 0)
    }
}
