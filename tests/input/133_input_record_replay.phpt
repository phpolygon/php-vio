--TEST--
Input record/replay: poll-tick clock, callbacks on replay, gamepads, JSON round-trip, scripts
--EXTENSIONS--
vio
--FILE--
<?php
function dump_events(array $events): void {
    foreach ($events as $e) {
        $tick = $e['tick']; $type = $e['type'];
        unset($e['tick'], $e['type']);
        echo "  $tick $type ", json_encode($e), "\n";
    }
}

$ctx = vio_create('null');
$log = [];
vio_on_key($ctx, function ($k, $a, $m) use (&$log) { $log[] = "key $k/$a/$m"; });
vio_on_char($ctx, function ($cp) use (&$log) { $log[] = "char $cp"; });

// ── Record ──────────────────────────────────────────────────────────
vio_virtual_gamepad_connect(1, 'Rec Pad');       // connected before start -> tick 0
vio_input_record_start($ctx);
vio_inject_key($ctx, VIO_KEY_A, VIO_PRESS, VIO_MOD_CONTROL);   // tick 0
vio_poll_events($ctx);
vio_inject_mouse_move($ctx, 10.0, 20.5);                       // tick 1
vio_inject_char($ctx, 'x');
vio_poll_events($ctx);
vio_inject_gamepad_button(1, VIO_GAMEPAD_B, VIO_PRESS);        // sampled at poll 3
vio_inject_gamepad_axis(1, VIO_GAMEPAD_AXIS_RIGHT_X, 0.25);
vio_poll_events($ctx);
vio_inject_scroll($ctx, 0.0, -1.0);                            // tick 3
vio_touch_inject($ctx, 7, VIO_TOUCH_BEGAN, 5.0, 6.0);
$events = vio_input_record_stop($ctx);
echo "recorded:\n";
dump_events($events);
var_dump(vio_input_record_stop($ctx));                         // nothing recording

// Reset the world the recording changed
vio_virtual_gamepad_disconnect(1);
vio_inject_key($ctx, VIO_KEY_A, VIO_RELEASE);
vio_touch_inject($ctx, 7, VIO_TOUCH_ENDED);
vio_begin($ctx); vio_end($ctx);
vio_inject_mouse_move($ctx, 0.0, 0.0);
$log = [];

// ── Replay (through JSON, as it would come from a file) ────────────
vio_input_replay($ctx, json_decode(json_encode($events), true));
echo "tick 0: ", implode(', ', $log), "\n";
var_dump(vio_input_replaying($ctx), vio_key_pressed($ctx, VIO_KEY_A), vio_gamepad_name(1));

// OS input is dropped while replaying; injections still pass
vio_poll_events($ctx);
echo "tick 1: ", implode(', ', $log), "\n";
var_dump(vio_mouse_position($ctx));

vio_poll_events($ctx);
var_dump(vio_gamepad_buttons(1)[VIO_GAMEPAD_B]);               // not yet
vio_poll_events($ctx);
var_dump(vio_gamepad_buttons(1)[VIO_GAMEPAD_B], vio_gamepad_axes(1)[VIO_GAMEPAD_AXIS_RIGHT_X]);
var_dump(vio_mouse_scroll($ctx), vio_touch_count($ctx));
var_dump(vio_input_replaying($ctx));                           // end delivered, still running

vio_poll_events($ctx);                                         // one poll later: finished
var_dump(vio_input_replaying($ctx), vio_gamepad_connected(1));

// ── Hand-written script: unsorted, same-tick order kept ─────────────
$log = [];
vio_input_replay($ctx, [
    ['tick' => 1, 'type' => 'key', 'key' => VIO_KEY_SPACE, 'action' => VIO_PRESS],
    ['tick' => 0, 'type' => 'char', 'codepoint' => 0x263A],
    ['tick' => 1, 'type' => 'key', 'key' => VIO_KEY_SPACE, 'action' => VIO_RELEASE],
]);
vio_poll_events($ctx);
echo implode(', ', $log), "\n";

// Stopping early hands input back and disconnects replay pads
vio_input_replay($ctx, [
    ['tick' => 0, 'type' => 'gamepad_connect', 'gamepad' => 5],
    ['tick' => 9, 'type' => 'end'],
]);
var_dump(vio_gamepad_name(5));
vio_input_replay_stop($ctx);
var_dump(vio_input_replaying($ctx), vio_gamepad_connected(5));

// ── Malformed entries: ValueError, nothing starts ───────────────────
foreach ([
    [['type' => 'key', 'key' => 65, 'action' => 1]],
    [['tick' => 0, 'type' => 'jump']],
    [['tick' => 0, 'type' => 'end'], ['tick' => 2, 'type' => 'key', 'key' => 65, 'action' => 9]],
    [['tick' => 0, 'type' => 'char', 'codepoint' => 10]],
    [['tick' => 0, 'type' => 'gamepad_axis', 'gamepad' => 16, 'axis' => 0, 'value' => 0]],
    ['nope'],
] as $bad) {
    try {
        vio_input_replay($ctx, $bad);
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}
var_dump(vio_input_replaying($ctx));

// vio_destroy mid-replay releases its pads, even while the object lives on
// (the caught ValueError's trace still references $ctx)
vio_input_replay($ctx, [['tick' => 0, 'type' => 'gamepad_connect', 'gamepad' => 2, 'name' => 'P'], ['tick' => 5, 'type' => 'end']]);
var_dump(vio_gamepad_connected(2));
vio_destroy($ctx);
var_dump(vio_gamepad_connected(2), vio_input_replaying($ctx));

// Freeing a context mid-replay releases them too
$ctx2 = vio_create('null');
vio_input_replay($ctx2, [['tick' => 0, 'type' => 'gamepad_connect', 'gamepad' => 2], ['tick' => 5, 'type' => 'end']]);
unset($ctx2);
var_dump(vio_gamepad_connected(2));
echo "OK\n";
?>
--EXPECT--
recorded:
  0 gamepad_connect {"gamepad":1,"name":"Rec Pad"}
  0 key {"key":65,"action":1,"mods":2}
  1 cursor {"x":10,"y":20.5}
  1 char {"codepoint":120}
  3 gamepad_button {"gamepad":1,"button":1,"pressed":true}
  3 gamepad_axis {"gamepad":1,"axis":2,"value":0.25}
  3 scroll {"dx":0,"dy":-1}
  3 touch {"id":7,"phase":1,"x":5,"y":6}
  3 end []
array(0) {
}
tick 0: key 65/1/2
bool(true)
bool(true)
string(7) "Rec Pad"
tick 1: key 65/1/2, char 120
array(2) {
  [0]=>
  float(10)
  [1]=>
  float(20.5)
}
bool(false)
bool(true)
float(0.25)
array(2) {
  [0]=>
  float(0)
  [1]=>
  float(-1)
}
int(1)
bool(true)
bool(false)
bool(false)
char 9786, key 32/1/0, key 32/0/0
string(15) "Virtual Gamepad"
bool(false)
bool(false)
vio_input_replay(): Argument #2 ($events) entry 0 needs an int 'tick' >= 0
vio_input_replay(): Argument #2 ($events) entry 0 has an unknown 'type'
vio_input_replay(): Argument #2 ($events) entry 1 needs an 'action' of VIO_RELEASE, VIO_PRESS or VIO_REPEAT
vio_input_replay(): Argument #2 ($events) entry 0 needs a printable 'codepoint'
vio_input_replay(): Argument #2 ($events) entry 0 needs a 'gamepad' slot between 0 and 15
vio_input_replay(): Argument #2 ($events) entry 0 must be an array
bool(false)
bool(true)
bool(false)
bool(false)
bool(false)
OK
