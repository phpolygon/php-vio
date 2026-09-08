--TEST--
Plugin registration via C-level test (constants and registry validation)
--EXTENSIONS--
vio
--FILE--
<?php
// Plugin registry is functional - verify via constants and list
var_dump(VIO_PLUGIN_API_VERSION === 1);
var_dump(VIO_PLUGIN_TYPE_GENERIC === 0);
var_dump(VIO_PLUGIN_TYPE_OUTPUT === 1);
var_dump(VIO_PLUGIN_TYPE_INPUT === 2);
var_dump(VIO_PLUGIN_TYPE_FILTER === 4);

// Bitmask combinations work
var_dump((VIO_PLUGIN_TYPE_OUTPUT | VIO_PLUGIN_TYPE_INPUT) === 3);
var_dump((VIO_PLUGIN_TYPE_OUTPUT | VIO_PLUGIN_TYPE_FILTER) === 5);
var_dump((VIO_PLUGIN_TYPE_INPUT | VIO_PLUGIN_TYPE_FILTER) === 6);
var_dump((VIO_PLUGIN_TYPE_OUTPUT | VIO_PLUGIN_TYPE_INPUT | VIO_PLUGIN_TYPE_FILTER) === 7);

// Registry starts empty (no native plugins registered by default)
$plugins = vio_plugins();
var_dump(is_array($plugins));
var_dump(count($plugins) === 0);

// Info for nonexistent
var_dump(vio_plugin_info("test_plugin") === false);
var_dump(vio_plugin_info("") === false);

echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK
