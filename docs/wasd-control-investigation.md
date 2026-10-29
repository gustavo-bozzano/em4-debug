# WASD control investigation

Target: the Steam `Em4.exe` whose SHA-256 is
`D7BF651648E3356F2066E9EB294BC65C06B323FB63299F7EB5A342E7A3DBA22B`.
All offsets below are relative to its `0x00400000` image base. The plugin checks
the executable timestamp and image size before accessing game objects.

## Observed native path

The script binding for `GameObject::PushActionMove(ActionInsertMode,
const Vector &, bool)` references native wrapper `0x00867CE0`. Its branch for
`direct_ == true` calls `0x00453140` with the native game object in `ECX`, an
action insert mode, a pointer to four floats, and a trailing zero. The native
routine reads the actor position at offsets `0xA8`, `0xAC`, and `0xB0`, then
builds a pathfinding action. The SDK declares `ACTION_NEWLIST` as enum value 4.

The binding for `GameObject::ClearActions()` passes the same native object to
`0x00432A20`. That routine clears the object's action queue. These findings
come from static analysis of the shipped binary. A prior live test confirmed
vehicle movement but also exposed a camera-key conflict. The relevant REA
Evidence IDs are
`ev_caa90afe5c3f5c19a416011260dcf09aca0f86ef0fa77a18fa8bba339b4887f1`,
`ev_5be32d257aefa1b561542f6c7d2d2a02c3f8842181d078483dd928bf71500333`,
`ev_0d39682d6ef008271afb6e7f1ae934b76edc5b9bca95b37f03a8e797252acfb5`,
and `ev_2704e430c98fc7550c5fcdb85ac7a757f5ed77c273d631dcfe3b8f0fa55be7f6`.

## Initial movement implementation

Ctrl+F8 arms control for exactly one selected vehicle or person. WASD picks a direction
relative to the active Vision camera. The initial implementation refreshed a
movement destination through the native action method while a key remained
held. Vehicles used a more distant target to reduce braking on diagonal input.
The controller clears the action queue when the keys are released, the game
loses focus, the selection changes, or control mode is turned off. Control
starts disabled and is intended for single-player testing.

The initial camera implementation used `Camera::FollowTarget` through the
native call at `0x005E4CC0`. In a live freeplay session, the controller's
follow flag and target pointer were set, but the active Vision camera stayed
stationary while the selected unit moved roughly 300 world units. This means
that setting the follow target alone does not move the camera rendered in that
mode. REA Evidence IDs for the original binding:
`ev_64216cc37db4bba5078241bab4d5d9b17a1fadf463b44e01d8ba8eb558b7b784`
and `ev_8a64e4a194f5a5debe8f8987db4997284300f860c5a62527bed166caafb71b2e`.

The follow implementation reads the active Vision view matrix each frame,
computes its camera position, and adds the selected unit's movement delta.
It sends that result through the native `Camera::Set` bridge at `0x008BD970`,
which calls the active game's camera virtual method and updates the engine's
camera position. Its static REA Evidence ID is
`ev_059f47fbdd7494a8d6184078f6a9b46ccc9037b2b135a2d4d408e6fc6e41efa7`.
Camera follow was confirmed visually for vehicles and persons. A further
change solves for the camera's horizontal translation that projects the
selected unit onto the center of the screen. It uses the current Vision
modelview and projection matrices, preserves camera height, and falls back to
movement-delta follow if the projection cannot be solved. The user confirmed
in a live test that the camera centered and followed both a vehicle and a
person, while vehicle movement remained smooth.

The updated DirectInput proxy intercepts keyboard `GetDeviceState` and
`GetDeviceData`, masking the WASD scan codes only while direct control is
active and the game has focus. The plugin still reads physical key state with
`GetAsyncKeyState`. The loader log confirmed that WASD input was masked in a
live session. Person movement worked in that session. Vehicle movement with
W and A was initially jerky; a more distant movement target and an eight-second
action interval fixed that behavior in the user's live test. The action still
updates immediately when the direction keys change.

This is directional pathfinding, not analog vehicle steering. Because action
replacement and input handling run inside the game process, test with a session
that can be restarted. Multiplayer synchronization has not been observed.

## Third-person camera prototype

The verified executable registers `Camera::LookAt` through its script binding.
Its native bridge at `0x008BDBC0` passes two vectors to `0x005E4850`, which
computes a look-at rotation and applies it to the active Vision camera object.
The plugin pairs this with the previously validated `Camera::Set` bridge to
place the camera behind the selected unit. REA Evidence IDs:
`ev_13ce5c001620211ab144a0e6886dcab3a936b020e6f936f93f7ba01b0604468c`
and `ev_9b74b65e09dd6aeb005d6fc035b0849f9976eef2e27ca44378943161ec8c1e15`.

An initial prototype called slot `0x38` on the selected native entity to get
its facing direction. This was incorrect: that slot belongs to a different
interface than the script wrapper's `GameObject::GetLookatDir`. On the native
entity it can be a burning-state method (`0x00437AB0`), and the game closed
when control was activated. The known-good DLL was restored immediately.
The corrected prototype derives heading from the unit's observed movement,
starting from the original camera's forward direction. It smooths camera
position and heading, uses that heading as the WASD movement frame, and
restores the original viewing direction and relative camera offset when
control ends. This version no longer closed the game, but its camera height
was too low for the level geometry. A read-only 15-second runtime sample showed
the active Vision eye descending from approximately Z=3907 to Z=-918 while
the game's camera-control Z reached zero; the resulting view was obstructed.
The final prototype derives chase height from the camera's initial elevation
above the unit and keeps the camera substantially higher. The user first confirmed in a
stationary vehicle test that both the vehicle and road were visible with the
revised height. In the subsequent live test, W, W+A, and D followed the chase
view, the camera stayed behind the vehicle during turns, Ctrl+F8 restored the
normal view, and the same camera mode worked for a person.

The follow-up smoothing update samples movement heading over 90 ms and filters
both the camera position and its focus point. It measures frame time with
`QueryPerformanceCounter` so the filter does not inherit the coarse steps of
`GetTickCount`. In a live vehicle test, the user reported that the camera
followed much more smoothly. The person mode and view restoration were not
separately confirmed in that follow-up test.

## Continuous vehicle movement and performance

Further live testing showed that repeated native vehicle move actions could
choose an unintended road or brake even while W remained held. Shorter targets,
queued targets, and an invisible follow target did not remove the braking. The
current controller uses the invisible target only for a short heading
calibration, then clears that action and moves the vehicle continuously through
native position and rotation methods. It checks terrain height and collisions.
The helper requires `mod:Prototypes/Objects/Misc/empty.e4p`. If it cannot be
created, the older pathfinding controller remains as a fallback.

The first direct-movement build reduced frame rate from roughly 130 FPS while
stopped to roughly 10 FPS while driving. Profiling found that scanning selected
units every rendered frame dominated the cost; the terrain, collision,
position, and rotation calls were comparatively small. The controller now
reuses the selected unit between periodic validity checks and updates vehicle
motion no more often than once every 12 ms. In the user's live test, W and
W+A remained smooth, the vehicle followed the intended street, and frame time
was approximately 5 ms stopped and 9 ms driving. The user accepted a later
12 ms movement-step build as smooth.

The same early-return change was applied to person movement, avoiding the
selection scan until the next native movement replan is due. The user confirmed
that person movement remained precise and the frame-rate drop improved. No
numerical person frame-time measurement was recorded. All these runtime tests
were in a single-player Blumenau session; multiplayer behavior is unverified.
