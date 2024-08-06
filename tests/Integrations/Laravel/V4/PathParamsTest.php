<?php

namespace DDTrace\Tests\Integrations\Laravel\V4;

use DDTrace\Tests\Integrations\Laravel\PathParamsTestSuite;

/**
 * @group appsec
 */
class PathParamsTest extends PathParamsTestSuite
{
    protected static function getAppIndexScript()
    {
        return __DIR__ . '/../../../Frameworks/Laravel/Version_4_2/public/index.php';
    }
}
