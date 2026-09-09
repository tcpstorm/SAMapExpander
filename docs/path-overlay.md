# The path overlay — seeing the graph the cars drive on

Every check on the imported cities' path graphs so far has been a number. The last run's numbers all
agree with each other:

```
car at (5958.3, 994.0, 7.5) is steering for (5954.9, 981.1); aiming at node 3920,
which this module puts at (5957.4, 980.2, 5.7) and whose lane point is (5954.9, 981.1).
car->dest 13.3, car->node 13.8, dest->node 2.7. mission 9, straight-line 255, lane 0 of 1,
link 3919->3920
```

`dest->node 2.7` is exactly the lane offset, the mission is holding, and the advance cadence is one
node per car per leg. And the driving still looks wrong. A tally cannot tell a graph that is in the
wrong place from a graph that is in the right place and badly followed — both read as "the numbers
agree" — so this draws the graph in the world instead.

Set `[World] Debug = 1`. Everything below is off until you press a key, and nothing installs under
SA-MP (the traffic, pedestrian and parked-car graphs are not loaded there either; the log says so).

## The keys

| key | draws |
|---|---|
| **F2** | the carriageway graph from `carnodes.dat` — nodes, links, and every lane point a node can hand a car |
| **F3** | the pedestrian graph from `pednodes.dat` — a different file, a different parser, the same origin |
| **F4** | every car this module is driving: where it is, where the *game* says it is steering, and the link the module believes it is on |
| **Ctrl+F2** | parked-car bays from `parkedcars.dat`, each an arrow pointing the way its car was parked |
| **Ctrl+F3** | ground probes: from each nearby carriageway node down to the surface `Ground::Under` finds there |
| **Ctrl+F4** | x-ray on/off. **On is the default**: lines draw through the world. Off lets the road hide a line |

F6–F9 are untouched. The overlay culls to **150 units around the camera**, not the player — fly up
in a helicopter or use the debug camera and you get a district at once. The log prints all six
bindings at start-up under `path overlay (debug lines, ...)`.

## What the colours mean

**Carriageway links are coloured by their own length**, because the length of a link is the quantity
every arrival test is measured against, and it is the current suspect.

| colour | length | what it means |
|---|---|---|
| **red** | under 2.5 | shorter than `kArrivedFloor` — a car arrives the instant it is aimed down it |
| **orange** | under 8.0 | shorter than `kArrivedDistance`, the old flat arrival radius |
| **yellow** | under 12.0 | shorter than `kMinLegLength`, the shortest leg the advance walk settles for |
| **green** | 12.0 and up | a leg a car can actually drive |

Everything else:

- **white ticks** — carriageway nodes. **Pale blue stubs** from each one — the lane points that node
  can produce, drawn by calling `LanePoint` itself. Their length is the 5.4-unit lane offset and
  their direction is which side of the road it lands on.
- **violet** — pedestrian links; **white lines** from a walking ped to the node it is heading for.
- **cyan, dark at one end and bright at the other** — the link a driven car believes it is on,
  bright at the end it is driving towards.
- **yellow line and ticks** — the car, and where the *vehicle's own autopilot destination* is (read
  from `+0x3EC`, the same bytes the log line reads). This is the line to watch.
- **orange square** round a node — the arrival radius for that car on that leg. If the square
  reaches the next node along, the car finishes the leg the moment it starts it.
- **purple stub** at the target node — the lane offset applied to it.
- **green vertical tick** — a node sitting on the ground probed under it (within a unit).
  **Magenta** — it is not. **A red spike straight down** — there is no surface under it at all, and
  no car will ever be placed there.
- **bays**: blue = armed and empty, green = a car is standing in it, grey = emptied or waiting to
  re-arm. An **amber line** from a bay is the disagreement between the height on its line and the
  height the collision actually has.

---

## 1. Does the graph lie on the roads?

Stand in a Vice City street. Press **F2**.

- **Expected**: green and yellow lines running down the middle of every road you can see, white
  ticks at the junctions, and pale blue stubs pointing off to one side.
- **Failure — the lines are somewhere else entirely.** The region's origin or the graph's
  coordinate space is wrong. Note *how far* off and *in which direction*: a constant offset in x
  and y is a wrong origin; an error that grows as you drive away from the region's own zero is a
  scale, and F8's "you are N units from the region's own zero" is the second reading that
  distinguishes them.
- **Failure — the lines run through buildings, or along where a road used to be.** The graph is
  the wrong city's, or the IPLs were placed at a different offset from the graph.
- **Failure — nothing is drawn at all.** Either the layer is off (the log and an on-screen message
  say which layers you toggled), or the graph is more than 150 units away, or `carnodes.dat` was
  never read — the start-up log says how many nodes and links each region got.
- **Failure — a road you can drive has no line on it.** Content, not code: the source game had no
  carriageway there.

Now press **Ctrl+F4** to turn x-ray off. Lines on the road surface will z-fight and flicker, which
is the graph telling you it is *at* the tarmac. Lines that vanish are **below** it, and lines that
still show clearly are **above** it.

## 2. Is the graph at the right height?

Press **Ctrl+F3** (with F2 on).

- **Expected**: short green ticks under the nodes near you, one or two units at most.
- **Failure — magenta ticks several units long.** The node is floating above or buried under the
  collision. Long magenta on a bridge or a flyover is expected; long magenta in an ordinary street
  is not.
- **Failure — red spikes.** `Ground::Under` found nothing at all. The collision for that block is
  not in memory, or there is a hole. Those coordinates are worth writing down.

This is deliberately capped at 40 probes within 45 units, because each one is a vertical line cast
through the world. If the frame rate drops noticeably with it on, turn it off — it has told you
what it can from where you are standing.

## 3. Are the cars following the graph? — **the important one**

Press **F4**, with **F2** still on, and stand where you can watch traffic.

Three pictures, three different faults:

- **The yellow line points along the cyan link, and the car moves along it.** The graph and the
  targeting are both right, and whatever is wrong is in how the car is *steered* — the autopilot,
  the cruise speed, the physics, the model. Nothing further will be learned from the graph, and the
  next place to look is `SetDestination` and the mission-holding code, not `carnodes.dat`.
- **The yellow line points somewhere the cyan link does not go.** The car is being driven towards
  something other than its own leg. Either the destination is being overwritten by the game's AI
  between our writes, or the lane point is on the wrong side.
- **The cyan links jump from one street to another every second or two, and the orange square is
  bigger than the gap to the next node.** The car is "arriving" before it has driven anywhere.
  That is the short-link problem, and F2's red and orange links say where it will happen.

Also worth watching:

- **The orange arrival square swallowing the next node.** Directly visible now.
- **The purple lane stub pointing into oncoming traffic, or onto the pavement.** The lane offset
  is being applied to the wrong side, or the source city drove on the other side.
- **The car marker sitting off the drawn road entirely.** The car is not where the graph put it —
  which is a spawn or a physics fault, not a routing one.

## 4. Pedestrians, as a control

Press **F3** with **F2** on.

The two graphs come out of two files through two parsers and share only the region origin. If the
violet lines land on the pavements and the carriageways do not land on the roads, the fault is in
the carriageway derivation. If **neither** lands anywhere sensible, the region's origin is wrong and
every other question here is downstream of that one.

## 5. Parked bays

Press **Ctrl+F2**.

- **Expected**: blue and green arrows lying in the kerbside spaces, pointing along the kerb.
- **Failure — arrows in the middle of the road, or pointing across it.** The heading conversion or
  the position is wrong.
- **Failure — long amber lines.** The height on the `parkedcars.dat` line and the height the
  collision has disagree; a car will be created at the probed height, not the written one.

---

## Cost, and when to turn it off

Every line is one draw call, because `CLines::RenderLineWithClipping` issues its own
`RwIm3DTransform`/`RwIm3DEnd` pair per segment. The budget is **1500 lines a frame**; past that the
picture truncates and, with `Debug = 1`, the log says
`path overlay: N line(s) ... BUDGET REACHED, the picture is truncated; fly closer` once a second.
Turning F2 on in a dense district with every layer lit will cost frames. That is the trade: this is
an instrument, not a feature.
