<?php

namespace DDTrace\Integrations\Symfony;

use ReflectionClass;
use ReflectionMethod;
use ReflectionAttribute;
use ReflectionException;

class PathExtractor
{
    protected $defaultRouteIndex = 0;
    protected $reader;
    protected $routeAnnotationClass = \Symfony\Component\Routing\Annotation\Route::class;

    public function __construct()
    {
        $this->reader = new \Doctrine\Common\Annotations\AnnotationReader(new \Doctrine\Common\Annotations\DocParser());
    }

    public function extract($classMethod, $routeName, $locale)
    {
        $className = $classMethod; //It may not come with method when invokable controller
        $methodName = null;
        if (str_contains($classMethod, "::")) {
            $exploded = explode("::", $classMethod);
            $className = $exploded[0];
            $methodName = $exploded[1];
        }

        if (!class_exists($className)) {
            return;
        }
        $class = new ReflectionClass($className);

        $globals = $this->getGlobals($class);
        if ($methodName == null) {
            if (!$class->hasMethod('__invoke')) {
                return;
            }
            $methodName = '__invoke';
            $globals = $this->resetGlobals();
        }

        try {
            $method = $class->getMethod($methodName);
        } catch (ReflectionException $e) {
            return;
        }

        $paths = [];
        $annotationsTarget = $methodName == '__invoke' ? $class: $method;

        foreach ($this->getAnnotations($annotationsTarget) as $annot) {
            $path = $this->getPath($annot, $globals, $class, $method, $routeName);
            if ($path !== null) {
                $paths = array_merge($paths, $path);
            }
        }
        if (!empty($paths) && is_array($paths)) {
            if (isset($paths[$locale])) {
                return $paths[$locale];
            } elseif (isset($paths[$this->defaultRouteIndex])) {
                return $paths[$this->defaultRouteIndex];
            }
            return reset($paths);
        }

        return;
    }

    private function getAnnotations(object $reflection): iterable
    {
        foreach ($reflection->getAttributes($this->routeAnnotationClass, ReflectionAttribute::IS_INSTANCEOF) as $attribute) {
            yield $attribute->newInstance();
        }

        if (!$this->reader) {
            return;
        }

        $annotations = $reflection instanceof \ReflectionClass
            ? $this->reader->getClassAnnotations($reflection)
            : $this->reader->getMethodAnnotations($reflection);

        foreach ($annotations as $annotation) {
            if ($annotation instanceof $this->routeAnnotationClass) {
                yield $annotation;
            }
        }
    }

    protected function getPath(object $annot, array $globals, ReflectionClass $class, ReflectionMethod $method, $routeName)
    {
        $name = $annot->getName() ?? $this->getDefaultRouteName($class, $method);
        $name = $globals['name'].$name;

        $path = $annot->getLocalizedPaths() ?: $annot->getPath();
        $prefix = $globals['localized_paths'] ?: $globals['path'];
        $paths = [];
        if (\is_array($path)) {
            if (!\is_array($prefix)) {
                foreach ($path as $locale => $localePath) {
                    if ($routeName ==  $name) {
                        $paths[$locale] = $prefix.$localePath;
                    }
                }
            } elseif (array_diff_key($prefix, $path)) {
                return;
            } else {
                foreach ($path as $locale => $localePath) {
                    if (!isset($prefix[$locale])) {
                        return;
                    }
                    if ($routeName ==  $name) {
                        $paths[$locale] = $prefix[$locale].$localePath;
                    }
                }
            }
        } elseif (\is_array($prefix)) {
            foreach ($prefix as $locale => $localePrefix) {
                if ($routeName ==  $name) {
                    $paths[$locale] = $localePrefix.$path;
                }
            }
        } elseif ($routeName ==  $name) {
            $path = $prefix.$path;
            $paths[] = empty($path) ? '/': $path;
        }

        return $paths;
    }

    private function resetGlobals(): array
    {
        return [
            'path' => null,
            'localized_paths' => [],
            'name' => '',
        ];
    }

    protected function getGlobals(ReflectionClass $class)
    {
        $globals = $this->resetGlobals();

        $annot = null;
        if ($attribute = $class->getAttributes($this->routeAnnotationClass, ReflectionAttribute::IS_INSTANCEOF)[0] ?? null) {
            $annot = $attribute->newInstance();
        }
        if (!$annot && $this->reader) {
            $annot = $this->reader->getClassAnnotation($class, $this->routeAnnotationClass);
        }

        if ($annot) {
            if (null !== $annot->getName()) {
                $globals['name'] = $annot->getName();
            }
            if (null !== $annot->getPath()) {
                $globals['path'] = $annot->getPath();
            }

            $globals['localized_paths'] = $annot->getLocalizedPaths();
        }

        return $globals;
    }

    protected function getDefaultRouteName(ReflectionClass $class, ReflectionMethod $method)
    {
        $name = str_replace('\\', '_', $class->name).'_'.$method->name;
        $name = \function_exists('mb_strtolower') && preg_match('//u', $name) ? mb_strtolower($name, 'UTF-8') : strtolower($name);
        if ($this->defaultRouteIndex > 0) {
            $name .= '_'.$this->defaultRouteIndex;
        }
        ++$this->defaultRouteIndex;

        $name = preg_replace('/(bundle|controller)_/', '_', $name);

        if (str_ends_with($method->name, 'Action') || str_ends_with($method->name, '_action')) {
            $name = preg_replace('/action(_\d+)?$/', '\\1', $name);
        }

        return str_replace('__', '_', $name);
    }

}
