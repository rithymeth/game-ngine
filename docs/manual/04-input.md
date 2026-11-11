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

Scripts can also read the host pointer with `Input.GetPointerPosition(index?)`.
It returns a vector whose `x` and `y` are window pixels and whose `z` is 1
when that pointer is active; pointer 0 is the mouse. `Input.IsPointerDown(index?)`
reports whether it is pressed. A press used by the UI is removed from its mapped
gameplay action before scripts see it.

Two conventions to know: a gamepad stick's Y is negative when pushed forward
(so the templates negate it onto a forward axis), and mouse movement is positive
right and down.

Add `Input/` to the project's **always cook** list, since no scene refers to
the bindings; the templates already do.
