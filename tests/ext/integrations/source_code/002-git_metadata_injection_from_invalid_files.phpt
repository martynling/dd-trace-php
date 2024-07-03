--TEST--
Basic Git Metadata Injection from invalid .git files (Repository URL & Commit Sha)
--ENV--
DD_TRACE_GENERATE_ROOT_SPAN=0
DD_TRACE_GIT_METADATA_ENABLED=1
--SKIPIF--
<?php
if (getenv('PHP_PEAR_RUNTESTS') === '1') die("skip: The pecl run-tests path is not in a git repository");
?>
--FILE--
<?php

require __DIR__ . '/../../includes/git_functions.inc';

ini_set('datadog.trace.git_metadata_enabled', 1);

generateInvalidFakeGitFolder(__DIR__);

function makeRequest() {
    /** @var \DDTrace\RootSpanData $rootSpan */
    $rootSpan = \DDTrace\start_span();
    \DDTrace\start_span();

    \DDTrace\close_span();
    \DDTrace\close_span();

    $closedSpans = dd_trace_serialize_closed_spans();
    $rootMeta = $closedSpans[0]['meta'];
    var_dump($rootMeta);

    \DDTrace\start_span();
    \DDTrace\close_span();

    $closedRoot = dd_trace_serialize_closed_spans();
    $rootMeta2 = $closedRoot[0]['meta'];
    var_dump($rootMeta2);
}

makeRequest();
makeRequest();

?>
--CLEAN--
<?php
system('rm -rf ' . __DIR__ . '/.git');
?>
--EXPECTF--
array(3) {
  ["runtime-id"]=>
  string(%d) "%s"
  ["_dd.p.dm"]=>
  string(2) "-0"
  ["_dd.p.tid"]=>
  string(16) "%s"
}
array(3) {
  ["runtime-id"]=>
  string(%d) "%s"
  ["_dd.p.dm"]=>
  string(2) "-0"
  ["_dd.p.tid"]=>
  string(16) "%s"
}
array(3) {
  ["runtime-id"]=>
  string(%d) "%s"
  ["_dd.p.dm"]=>
  string(2) "-0"
  ["_dd.p.tid"]=>
  string(16) "%s"
}
array(3) {
  ["runtime-id"]=>
  string(%d) "%s"
  ["_dd.p.dm"]=>
  string(2) "-0"
  ["_dd.p.tid"]=>
  string(16) "%s"
}
