<?php

/**
 * @file
 * Akismet client class for Drupal.
 */

/**
 * Drupal Akismet client implementation.
 */
class AkismetDrupal extends Akismet {

  /**
   * Overrides the connection timeout based on module configuration.
   *
   * @see Akismet::__construct().
   */
  public function __construct() {
    parent::__construct();
    $this->requestTimeout = variable_get('akismet_connection_timeout', 3);
  }

  /**
   * Mapping of configuration names to Drupal variables.
   *
   * @see Akismet::loadConfiguration()
   */
  public $configuration_map = array(
    'key' => 'akismet_api_key',
  );

  /**
   * Implements Akismet::loadConfiguration().
   */
  public function loadConfiguration($name) {
    $name = $this->configuration_map[$name];
    return variable_get($name);
  }

  /**
   * Implements Akismet::saveConfiguration().
   */
  public function saveConfiguration($name, $value) {
    // Set local variable.
    if (property_exists('AkismetDrupal', $name)) {
      $this->{$name} = $value;
    }
    // Persist in Drupal.
    $name = $this->configuration_map[$name];
    variable_set($name, $value);
  }

  /**
   * Implements Akismet::deleteConfiguration().
   */
  public function deleteConfiguration($name) {
    $name = $this->configuration_map[$name];
    variable_del($name);
  }

  /**
   * Implements Akismet::getUserAgentInformation().
   */
  public function getUserAgentInformation() {
    // Retrieve Drupal distribution and installation profile information.
    $profile = drupal_get_profile();
    $profile_info = system_get_info('module', $profile) + array(
      'distribution_name' => 'Drupal',
      'version' => VERSION,
    );

    // Retrieve Akismet module information.
    $akismet_info = system_get_info('module', 'akismet');
    if (empty($akismet_info['version'])) {
      // Manually build a module version string for repository checkouts.
      $akismet_info['version'] = DRUPAL_CORE_COMPATIBILITY . '-1.x-dev';
    }

    $data = array(
      'platformName' => $profile_info['distribution_name'],
      'platformVersion' => $profile_info['version'],
      'clientName' => $akismet_info['name'],
      'clientVersion' => $akismet_info['version'],
    );
    return $data;
  }

  /**
   * Overrides Akismet::getSiteURL().
   */
  public function getSiteURL() {
    return $GLOBALS['base_url'];
  }

  /**
   * Overrides Akismet::writeLog().
   */
  function writeLog() {
    foreach ($this->log as $entry) {
      $entry['Request: ' . $entry['request']] = !empty($entry['data']) ? $entry['data'] : NULL;
      unset($entry['request'], $entry['data']);

      $entry['Request headers:'] = $entry['headers'];
      unset($entry['headers']);

      $entry['Response: ' . $entry['response_code'] . ' ' . $entry['response_message'] . ' (' . number_format($entry['response_time'], 3) . 's)'] = $entry['response'];
      unset($entry['response'], $entry['response_code'], $entry['response_message'], $entry['response_time']);

      // The client class contains the logic for recovering from certain errors,
      // and log messages are only written after that happened. Therefore, we
      // can normalize the severity of all log entries to the overall success or
      // failure of the attempted request.
      // @see Akismet::query()
      akismet_log($entry, $this->lastResponse->isError ? WATCHDOG_ERROR : NULL);
    }

    // After writing log messages, empty the log.
    $this->purgeLog();
  }

  /**
   * Implements Akismet::request().
   */
  protected function request($method, $server, $path, $query = NULL, array $headers = array()) {
    $request = array(
      'method' => $method,
      'headers' => $headers,
      'timeout' => $this->requestTimeout,
    );
    if (isset($query)) {
      if ($method == 'GET') {
        $path .= '?' . $query;
      }
      elseif ($method == 'POST') {
        $request['data'] = $query;
      }
    }

    $dhr = drupal_http_request($server . '/' . $path, $request);
    // @todo Core: Ensure that $dhr->code is an integer.
    $dhr->code = (int) $dhr->code;
    // @todo Core: Any other code than 200 is interpreted as error.
    if ($dhr->code >= 200 && $dhr->code < 300) {
      unset($dhr->error);
    }
    // @todo Core: data property is not assigned if there is no response body.
    if (!isset($dhr->data)) {
      $dhr->data = NULL;
    }
    // @todo Core: Timeout produces a bogus non-negative status code.
    // @see http://drupal.org/node/1246376
    if ($dhr->code === 1) {
      $dhr->code = -1;
    }

    $response = (object) array(
      'code' => $dhr->code,
      'message' => isset($dhr->error) ? $dhr->error : NULL,
      'headers' => isset($dhr->headers) ? $dhr->headers : array(),
      'body' => $dhr->data,
    );
    return $response;
  }
}

/**
 * Drupal Akismet client implementation using test parameter.
 */
class AkismetDrupalTest extends AkismetDrupal {

  /**
   * {@inheritdoc}
   */
  public function query($method, $path, $data, $authenticate = TRUE) {
    $data['is_test'] = 1;
    return parent::query($method, $path, $data, $authenticate);
  }
}
