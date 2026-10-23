import ApexCore
import SwiftUI

/// The knob mock's palette and motion (bridge/index.html `:root`, `.buzz`, `@keyframes pulse`).
enum Theme {
    static let working = Color(red: 0x4c / 255, green: 0xc3 / 255, blue: 0xff / 255)
    static let attention = Color(red: 0xff / 255, green: 0x7a / 255, blue: 0x1a / 255)
    static let done = Color(red: 0x3d / 255, green: 0xdc / 255, blue: 0x84 / 255)
    static let idle = Color(red: 0x5d / 255, green: 0x5b / 255, blue: 0x57 / 255)
    static let ink = Color(red: 0xf1 / 255, green: 0xef / 255, blue: 0xe9 / 255)
    static let muted = Color(red: 0x8a / 255, green: 0x87 / 255, blue: 0x80 / 255)
    static let detail = Color(red: 0xc9 / 255, green: 0xc6 / 255, blue: 0xbf / 255)

    static func color(_ bucket: Bucket?) -> Color {
        switch bucket {
        case .working: return working
        case .attention: return attention
        case .done: return done
        case .idle, .none: return idle
        }
    }
}

/// The mock's buzz: a short sideways shake each time `travel` grows by one.
struct Shake: GeometryEffect {
    var travel: CGFloat

    var animatableData: CGFloat {
        get { travel }
        set { travel = newValue }
    }

    func effectValue(size: CGSize) -> ProjectionTransform {
        ProjectionTransform(CGAffineTransform(translationX: 3 * sin(travel * .pi * 4), y: 0))
    }
}

/// The attention pulse: the view breathes while `enabled`.
struct Pulse: ViewModifier {
    let enabled: Bool

    func body(content: Content) -> some View {
        if enabled {
            content.phaseAnimator([false, true]) { view, bright in
                view.opacity(bright ? 1 : 0.4)
            } animation: { _ in
                .easeInOut(duration: 0.55)
            }
        } else {
            content
        }
    }
}

/// Double Tap (watchOS 11+) presses the button it is on. `isEnabled` keeps the view identity stable.
struct DoubleTap: ViewModifier {
    let enabled: Bool

    func body(content: Content) -> some View {
        if #available(watchOS 11.0, *) {
            content.handGestureShortcut(.primaryAction, isEnabled: enabled)
        } else {
            content
        }
    }
}

/// The background tinted by the agent's state: a soft glow that drifts slowly ("aurora") — blue while working,
/// orange and breathing while it needs you, calm green when done, nothing when idle. `time` drives it.
struct Aurora: View {
    let bucket: Bucket?
    let time: Double

    var body: some View {
        GeometryReader { geometry in
            let size = geometry.size
            let color = Theme.color(bucket)
            let strength: Double = {
                switch bucket {
                case .attention: return 0.24 + 0.10 * sin(time * 2.6)
                case .working: return 0.20
                case .done: return 0.14
                case .idle, .none: return 0
                }
            }()
            ZStack {
                RadialGradient(colors: [color.opacity(strength), .clear],
                               center: UnitPoint(x: 0.5 + 0.22 * cos(time * 0.23), y: 0.42 + 0.16 * sin(time * 0.31)),
                               startRadius: 0, endRadius: max(size.width, size.height) * 0.75)
                RadialGradient(colors: [color.opacity(strength * 0.55), .clear],
                               center: UnitPoint(x: 0.5 - 0.25 * sin(time * 0.19), y: 0.7 + 0.12 * cos(time * 0.27)),
                               startRadius: 0, endRadius: max(size.width, size.height) * 0.6)
            }
        }
        .allowsHitTesting(false)
    }
}

/// Reports the corner radius on `edge` that is concentric with the display's corners for the view it is on —
/// `GeometryProxy.concentricCornerRadii` (watchOS 27): the container's corner radius minus the distance to
/// it. Before watchOS 27, or without a container shape, `fallback`.
///
/// A value below `minimum` is a transient, not the display: during a launch or resume the real watch once
/// reported 8 pt for a track that measures 57, and the track stuck with near-square corners that the display
/// cut off. Such a value keeps the last good radius and goes to `onTransient` instead.
struct ConcentricRadius: ViewModifier {
    var edge: VerticalEdge = .bottom
    let fallback: CGFloat
    @Binding var radius: CGFloat
    var minimum: CGFloat = 24
    var onTransient: ((CGFloat) -> Void)? = nil

    func body(content: Content) -> some View {
        if #available(watchOS 27.0, *) {
            content.onGeometryChange(for: CGFloat.self) { proxy in
                guard let radii = proxy.concentricCornerRadii else { return 0 }
                return edge == .top
                    ? max(radii.topLeading, radii.topTrailing)
                    : max(radii.bottomLeading, radii.bottomTrailing)
            } action: { measured in
                if measured >= minimum {
                    radius = measured
                } else if measured > 1 {
                    onTransient?(measured)
                } else {
                    radius = fallback
                }
            }
        } else {
            content
        }
    }
}
