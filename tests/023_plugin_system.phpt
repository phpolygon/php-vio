--TEST--
Plugin system API
--EXTENSIONS--
vio
--FILE--
<?php
// Plugin registry should be initialized (empty)
$plugins = vio_plugins();
var_dump(is_array($plugins));
var_dump(count($plugins) === 0);

// Plugin info for nonexistent plugin
var_dump(vio_plugin_info('nonexistent'));

// Plugin type constants
var_dump(defined('VIO_PLUGIN_TYPE_GENERIC'));
var_dump(defined('VIO_PLUGIN_TYPE_OUTPUT'));
var_dump(defined('VIO_PLUGIN_TYPE_INPUT'));
var_dump(defined('VIO_PLUGIN_TYPE_FILTER'));
var_dump(defined('VIO_PLUGIN_API_VERSION'));

var_dump(VIO_PLUGIN_TYPE_GENERIC === 0);
var_dump(VIO_PLUGIN_TYPE_OUTPUT === 1);
var_dump(VIO_PLUGIN_TYPE_INPUT === 2);
var_dump(VIO_PLUGIN_TYPE_FILTER === 4);
var_dump(VIO_PLUGIN_API_VERSION === 1);

echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(false)
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
