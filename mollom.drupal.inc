<?php

/**
 * @file
 * Mollom client class for Drupal.
 */

/**
 * Drupal Mollom client implementation.
 */
class MollomDrupal extends Mollom {
  /**
   * Mapping of configuration names to Drupal variables.
   *
   * @see Mollom::loadConfiguration()
   */
  private $configuration_map = array(
    'publicKey' => 'mollom_public_key',
    'privateKey' => 'mollom_private_key',
    'servers' => 'mollom_servers',
  );

  /**
   * Implements Mollom::loadConfiguration().
   */
  public function loadConfiguration($name) {
    $name = $this->configuration_map[$name];
    return variable_get($name);
  }

  /**
   * Implements Mollom::saveConfiguration().
   */
  public function saveConfiguration($name, $value) {
    $name = $this->configuration_map[$name];
    return variable_set($name, $value);
  }

  /**
   * Implements Mollom::deleteConfiguration().
   */
  public function deleteConfiguration($name) {
    $name = $this->configuration_map[$name];
    return variable_del($name);
  }

  /**
   * Implements Mollom::getClientInformation().
   */
  public function getClientInformation() {
    if ($cache = cache_get('mollom_version')) {
      return $cache->data;
    }

    // Retrieve Drupal distribution and installation profile information.
    $profile = drupal_get_profile();
    $profile_info = system_get_info('module', $profile) + array(
      'distribution_name' => 'Drupal',
      'version' => VERSION,
    );

    // Retrieve Mollom module information.
    $mollom_info = system_get_info('module', 'mollom');
    if (empty($mollom_info['version'])) {
      // Manually build a module version string for repository checkouts.
      $mollom_info['version'] = DRUPAL_CORE_COMPATIBILITY . '-1.x-dev';
    }

    $data = array(
      'platformName' => $profile_info['distribution_name'],
      'platformVersion' => $profile_info['version'],
      'clientName' => $mollom_info['name'],
      'clientVersion' => $mollom_info['version'],
    );
    cache_set('mollom_version', $data);

    return $data;
  }

  /**
   * Overrides Mollom::writeLog().
   */
  function writeLog() {
    $messages = array();
    foreach ($this->log as $i => $entry) {
      $entry += array('arguments' => array());
      $message = array(
        $entry['message'] => $entry['arguments'],
      );
      if (isset($entry['data'])) {
        $message['Request: @request<pre>@parameters</pre>'] = array(
          '@request' => $entry['request'],
          '@parameters' => $entry['data'],
        );
        unset($entry['request'], $entry['data']);
      }
      if (isset($entry['response'])) {
        $message['Response:<pre>@response</pre>'] = array(
          '@response' => $entry['response'],
        );
      }
      $messages[] = $message;

      // Translate log messages for debugging without watchdog.
      // @todo Move into mollom.unit.inc implementation.
      /*
      $output = array();
      foreach ($message as $text => $args) {
        foreach ($args as &$arg) {
          if (is_array($arg)) {
            $arg = var_export($arg, TRUE);
          }
        }
        $output[] = strtr($text, $args);
      }
      $this->log[$i]['message'] = implode("\n", $output);
      unset($this->log[$i]['arguments']);
      drupal_set_message(implode('<br />', $output), $this->log[$i]['severity']);
      */
    }
    _mollom_watchdog_multiple($messages, $this->lastResponseCode === TRUE ? WATCHDOG_DEBUG : WATCHDOG_ERROR);

    // After writing log messages, empty the log.
    $this->purgeLog();
  }

  /**
   * Overrides Mollom::refreshServers().
   */
  protected function refreshServers() {
    $servers = parent::refreshServers();

    // Allow other modules to alter the server list. Internal use only.
    drupal_alter('mollom_server_list', $servers);

    return $servers;
  }

  /**
   * Implements Mollom::request().
   */
  protected function request($method, $server, $path, $query = NULL, array $headers = array()) {
    $request = array(
      'method' => $method,
      'headers' => $headers,
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
 * Drupal Mollom client implementation using production testing servers.
 */
class MollomDrupalTest extends MollomDrupal {
  /**
   * Overrides Mollom::$serversInit.
   */
  public $serversInit = array('http://rest.mollom.com');

  /**
   * Overrides Mollom::refreshServers().
   *
   * @todo FIXME: Site data is not consistent across servers. Testing site
   *   record is initially created on first hard-coded server.
   *   MollomServerListRecoveryTestCase replaces server list with invalid URLs
   *   to verify proper recovery. Since there is no server list,
   *   refreshServers() is invoked, which calls GET /site/$publicKey, which
   *   contains a new server list. The first server in that list is used for
   *   subsequent/following requests. But the first server in the returned list
   *   can be different to the first server in our hard-coded list
   *   (Mollom::serversInit), and in that case, authentication fails, and in
   *   turn, all tests fail.
   */
  protected function refreshServers() {
    // Retrieve new server list.
    $servers = parent::refreshServers();
    // Nothing to do on empty list.
    if (empty($servers)) {
      return $servers;
    }

    // Append API version to serversInit.
    $known = array();
    foreach ($this->serversInit as $server) {
      $known[] = $server . '/' . self::API_VERSION;
    }
    // Check whether any of the hard-coded servers is contained, and if so, sort
    // them first (and in their hard-coded order).
    $new = array_intersect($known, $servers);
    // Append the remaining.
    $new = array_merge($new, array_diff($servers, $new));

    return $new;
  }
}

/**
 * Drupal Mollom client implementation using local dummy/fake REST server.
 */
class MollomDrupalTestLocal extends MollomDrupal {
  /**
   * Overrides Mollom::__construct().
   */
  function __construct() {
    parent::__construct();
    // Replace initial server list with local fake server.
    $this->serversInit = array($GLOBALS['base_url'] . '/mollom-test/rest');
  }
}

