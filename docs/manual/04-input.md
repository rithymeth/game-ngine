# Input

Input is **actions** and **mapping contexts**.

- An **action** (`Move`, `Jump`) has a value type: bool, 1D, 2D or 3D axis.
- A **mapping context** (`Gameplay`) binds physical keys, mouse axes and gamepad
  buttons and sticks to actions. Each binding can have **modifiers** (negate,
  swizzle, dead zone, scale) and **triggers** (down, pressed, released, hold,
  tap, double tap, chord).

The templates ship a `Gameplay` context: WASD, arrow keys and the left stick on
`Move`; Space and the A button on `Jump`. Both are plain files
(`Content/Input/*.aaction`, `*.amapping`), edited in the editor or by hand.

In a script: `Input.GetAxis2D("Move")`, `Input.IsTriggered("Jump")`, or
`Input.OnStarted("Jump")` for an event you connect to. In C++:
`InputSystem::GetAxis2D`, `GetAction`.

Two conventions to know: a gamepad stick's Y is negative when pushed forward
(so the templates negate it onto a forward axis), and mouse movement is positive
right and down.

Add `Input/` to the project's **always cook** list, since no scene refers to
the bindings; the templates already do.
