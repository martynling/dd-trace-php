<?php

namespace DDTrace\Tests\Integrations\WordPress\V6_1;

use DDTrace\Tests\Integrations\WordPress\PathParamsTestSuite;

 /**
 * @group appsec
 */
class PathParamsTest extends PathParamsTestSuite
{
    protected static function getAppIndexScript()
    {
        return __DIR__ . '/../../../Frameworks/WordPress/Version_6_1/index.php';
    }
}
