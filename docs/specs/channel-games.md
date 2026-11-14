# Channel games

Scope owner: SS-169. The first game is an ICQ RPS-inspired tactical board game,
reinterpreted with fire, water and ice. The owner confirmed these reconstructed
rules on 2026-11-14.

## Shared experience

Only an owned SquadSpeak channel offers starting a game. The channel owner starts
and participates. Each channel has at most one active game. Each user can
participate in at most one game at a time across channels and hosts. Starting or
joining another game must not silently leave an existing one. Any approved chat
participant can join without changing their voice connection. External providers
and private conversations do not offer this action.

The chat shows an invitation with the inviter, game name, icon and Join action.
Join is available only while a place is free. The channel also shows the game icon.
The invitation stays in chat, while play opens in a separate game window.
Reopening the same game focuses its existing window. Starting, joining, setup,
readiness, turns and ending are authoritative host events. Repeated or stale
actions must not create extra players, turns, games or windows.

After joining, each player sees the board and a short explanation. Before actual
readiness they must select their trap and protected target on two distinct home
positions, hidden from their opponent. An introductory countdown must never
choose these positions or bypass valid setup. Play starts only after both players
have completed setup and become ready. Leave is always available to participants.
Leaving or losing either player's connection ends this two-player game with a
visible reason. In later games, the game's rules define how non-owner departures
affect play. Owner departure always ends a game.

Games are live channel activities. They reuse authenticated encrypted chat
transport and its access rules. They do not add a game server, database or account.
Restarting a host ends its game. Invitation and result cards exist only during
the current session and are not written to chat history. The client keeps at most
160 session cards, matching the chat window budget. A live invitation remains
reachable while browsing earlier messages.
Late readers receive the current snapshot. Unknown optional game capabilities
must preserve baseline chat under SS-165. Voice and chat remain usable during play.

## Elemental board rules

These confirmed rules define SquadSpeak's game. They do not claim that every
historical ICQ detail has been recovered:

- Two players use a board seven columns wide and six rows high. Each starts with
  fourteen pieces in their two home rows.
- Each player selects one immobile target and one immobile trap. The
  neutral target name is **Core** (German **Kern**). The other twelve positions
  receive four fire, four water and four ice pieces in random order.
- Players alternate moving one elemental piece one square horizontally or
  vertically. No diagonal move or jump is allowed.
- Water defeats fire, fire defeats ice, and ice defeats water. The losing piece
  is removed. Equal elements require both players to choose a new element
  privately, repeating if necessary until there is a winner.
- Opposing piece roles remain hidden until combat reveals them. The surviving
  piece's revealed element remains visible. Private setup and pending choices
  must never be included in the opponent's or an uninvolved reader's snapshot.
- A trap removes an attacker and remains on its square. Capturing the opposing
  Core or eliminating their movable pieces wins the match.

Fire, water and ice have distinct shapes as well as colors. Short melting,
extinguishing and freezing effects explain outcomes without delaying actions or
obscuring the board. Reduced motion uses a static outcome instead. Artwork and
effects follow SquadSpeak's style and do not reuse ICQ assets. Pieces use detailed,
dimensional character artwork matching the avatars, with transparent silhouettes.
Concealed opponents share one neutral character with no role clues. Selection,
ownership markers and shadows are rendered separately by the app. All figures
must remain distinct and uncropped in compact and large windows.

The host remains the trusted authority, as for channel chat. This feature does
not introduce a separate game service or claim protection against a modified
host inspecting its own authoritative game state.

## Research and scope

[Pocket Gamer's contemporary 2008 review](https://www.pocketgamer.com/rock-paper-scissors/review/)
describes the ICQ-linked mobile edition: fourteen pieces per side, twelve armed
pieces, player-selected trap and flag bearer, single-square turns, initially
hidden enemy weapons and a new choice when equal weapons meet. It is a firsthand
review of the mobile adaptation, not an original PC rules manual.

The current [RPS Ninja developer guide](https://rpsninja.uk/landing/de) documents
a classic 7×6 board, hidden setup, persistent traps and combat disclosure.
[Helpiqo's own rules](https://helpiqo.com/en/rps-duel) explicitly specify four
pieces per element, orthogonal movement and immobile flag/trap pieces. These are
modern remakes and evidence for the proposed reconstruction, not authority for
every original ICQ detail.

The exact historical turn timeout, trap behavior and post-tie weapon persistence have not
been independently established from an original PC manual. Do not silently add
timeouts, advanced modes, rankings, spectators or additional games.

After valid placement the ten-second introduction countdown begins. Ready skips
it and locks setup. Closing the game window leaves the game, just like Leave.
Changing setup before readiness restarts the countdown. Movement alone never
reveals an opponent's element. A tie changes the two fighting pieces to their
newly chosen elements. No per-turn timeout is added.

## Acceptance

Exercise invitation, approved chat-only join, full lobby, concurrent joins and
cross-channel/host attempts to join a second game. Verify private setup, invalid
or overlapping role selections, premature Ready, legal/illegal moves, alternating
turns, all elemental outcomes, hidden tie choices, trap and target encounters,
replayed commands and terminal outcomes through the actual UI and TLS path.
Verify Leave, window reopening, disconnect, host stop, late readers and restart.
Reject unauthorized starts, unrelated channels, private/external chats and invalid
commands. Check compact and large game windows, keyboard/touch controls, reduced
motion and unchanged voice/chat participation.

## Later candidates

These use original SquadSpeak artwork and styling, not copied game assets:

- Light trails: steer a trail and avoid walls and other trails.
- Multiplayer Snake: collect food while avoiding growing snakes.
- Four in a row: a compact two-player turn-based game.
- Reversi: two players capture pieces by enclosing them.
- Dots and boxes: take turns completing little squares.
- Memory pairs: a shared board with alternating turns.
- Reaction duel: react to a signal, with early presses counting as a miss.
- Drawing guesses: one player draws while others guess in a game-specific input.

Small turn-based games are the simplest next addition. Real-time trails and Snake
need separate latency, input timing and capacity qualification. Reference examples
include [Board Game Arena's Connect Four](https://en.boardgamearena.com/gamepanel?game=connectfour)
and [AirConsole's group games](https://www.airconsole.com/games/games-for-groups).
