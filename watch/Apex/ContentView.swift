import SwiftUI

/// Three horizontal pages: Settings ← (swipe right) the main page → (swipe left) the chosen worktree's agent.
/// The main page is the knob or the cards (Settings → Layout). Horizontal paging leaves the crown to the page
/// on screen (`.verticalPage` would take it from the knob). No toolbar, no page dots and no system clock: the
/// time is no use here, so the whole display belongs to the worktrees and the agent's message.
struct ContentView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var keepWatching: KeepWatching
    @AppStorage(Preferences.layoutKey) private var layout = Layout.knob
    @State private var page = Page.main

    enum Page: Hashable {
        case settings, main, agent
    }

    var body: some View {
        pages
            // watchOS has no public modifier for the clock; this one is soft-deprecated, so it builds clean.
            ._statusBarHidden(true)
    }

    @ViewBuilder
    private var pages: some View {
        switch DebugLaunch.page {
        case 1:
            // DEBUG screenshots only: a hidden-index page TabView ignores programmatic selection, and
            // simctl cannot swipe — so the requested page is rendered on its own.
            AgentView(model: model, active: true)
                .onAppear { model.agentPageVisible = true }
        case 2:
            SettingsView(model: model, keepWatching: keepWatching)
        default:
            pager
        }
    }

    private var pager: some View {
        TabView(selection: $page) {
            SettingsView(model: model, keepWatching: keepWatching)
                .tag(Page.settings)
            main
                .tag(Page.main)
            AgentView(model: model, active: page == .agent)
                .tag(Page.agent)
        }
        .tabViewStyle(.page(indexDisplayMode: .never))
        .toolbar(.hidden, for: .navigationBar)
        .onChange(of: page) { _, value in model.agentPageVisible = value == .agent }
        // After a send or an answer the main page comes back, turned to the next agent waiting on the user.
        .onChange(of: model.homeRequest) { _, _ in withAnimation { page = .main } }
    }

    @ViewBuilder
    private var main: some View {
        switch layout {
        case .knob:
            KnobView(model: model, keepWatching: keepWatching, active: page == .main)
        case .cards:
            CardsView(model: model, keepWatching: keepWatching, active: page == .main)
        }
    }
}
