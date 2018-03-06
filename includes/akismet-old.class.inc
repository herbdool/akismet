<?php

/**
 * @file
 * Akismet client class.
 *
 * @license MIT|GNU GPL v2
 *   See LICENSE-MIT.txt or LICENSE-GPL.txt shipped with this library.
 */

/**
 * The base class for Akismet client implementations.
 */
abstract class Akismet {
  /**
   * The Akismet API version, used in HTTP requests.
   */
  const API_VERSION = 'v1';

  /**
   * Network communication failure code: Server could not be reached.
   *
   * @see AkismetNetworkException
   */
  const NETWORK_ERROR = 900;

  /**
   * Server communication failure code: Unexpected server response.
   *
   * Using the 5xx HTTP status code range, but not re-using an existing HTTP
   * code to prevent bogus bug reports. 511 is the closest comparable code
   * 501 (Not Implemented) plus 10.
   *
   * @see AkismetResponseException
   */
  const RESPONSE_ERROR = 511;

  /**
   * Client communication failure code: Bad request.
   *
   * @see AkismetBadRequestException
   */
  const REQUEST_ERROR = 400;

  /**
   * Client communication failure code: Authentication error.
   *
   * @see AkismetAuthenticationException
   */
  const AUTH_ERROR = 401;

  /**
   * The Akismet API key to use for request authentication.
   *
   * @var string
   */
  public $key = '';

  /**
   * The Akismet server to communicate with, without the API key subdomain.
   *
   * @var string
   */
  public $server = 'rest.akismet.com';

  /**
   * Maximum number of attempts for a request to a Akismet server.
   *
   * @see Akismet::query()
   * @see Akismet::$requestTimeout
   *
   * @var integer
   */
  public $requestMaxAttempts = 2;

  /**
   * Seconds in which a request to the Akismet server times out.
   *
   * The timeout applies per request. Akismet::query() will retry a request until
   * it reaches Akismet::$requestMaxAttempts. With the default values, a Akismet
   * API call has a total timeout of 6 seconds in case of a server failure.
   *
   * @see Akismet::request()
   * @see Akismet::$requestMaxAttempts
   *
   * @var float
   */
  public $requestTimeout = 3.0;

  /**
   * The status code of the last server response, or TRUE if the request succeeded.
   *
   * If not TRUE, then the value is either one of
   * - Akismet::NETWORK_ERROR
   * - Akismet::RESPONSE_ERROR
   * - Akismet::REQUEST_ERROR
   * - Akismet::AUTH_ERROR
   * or the actual HTTP status code returned by the server.
   *
   * @var int|bool|null
   */
  public $lastResponseCode = NULL;

  /**
   * Flag indicating whether to invoke Akismet::writeLog() in Akismet::query().
   *
   * @var bool
   */
  public $writeLog = TRUE;

  /**
   * A list of logged requests.
   *
   * @var array
   */
  public $log = array();

  function __construct() {
    $this->key = $this->loadConfiguration('key');
  }

  /**
   * Loads a configuration value from client-side storage.
   *
   * @param string $name
   *   The configuration setting name to load. Currently the only possibility is
   *   - key: The API key for Akismet authentication.
   *
   * @return mixed
   *   The stored configuration value or NULL if there is none.
   *
   * @see Akismet::saveConfiguration()
   * @see Akismet::deleteConfiguration()
   */
  abstract protected function loadConfiguration($name);

  /**
   * Saves a configuration value to client-side storage.
   *
   * @param string $name
   *   The configuration setting name to save.
   * @param mixed $value
   *   The value to save.
   *
   * @see Akismet::loadConfiguration()
   * @see Akismet::deleteConfiguration()
   */
  abstract protected function saveConfiguration($name, $value);

  /**
   * Deletes a configuration value from client-side storage.
   *
   * @param string $name
   *   The configuration setting name to delete.
   *
   * @see Akismet::loadConfiguration()
   * @see Akismet::saveConfiguration()
   */
  abstract protected function deleteConfiguration($name);

  /**
   * Returns platform and version information about the Akismet client.
   *
   * Retrieves platform and Akismet client version information to send along to
   * Akismet in the User-Agent header.
   *
   * @return array
   *   An associative array containing:
   *   - platformName: The name of the platform/distribution; e.g., "Drupal".
   *   - platformVersion: The version of platform/distribution; e.g., "7.0".
   *   - clientName: The official Akismet client name; e.g., "Akismet".
   *   - clientVersion: The version of the Akismet client; e.g., "7.x-1.0".
   */
  abstract public function getClientInformation();

  /**
   * Writes log messages to a permanent location/storage.
   *
   * Not abstract, since clients are not required to write log messages.
   * The log may be written and appended to a file (via file_put_contents()),
   * syslog (on *nix-based systems), or a database.
   *
   * @see Akismet::log
   */
  public function writeLog() {
    // After writing log messages, empty the log.
    $this->purgeLog();
  }

  /**
   * Purges captured log messages.
   *
   * @see Akismet::writeLog()
   */
  final public function purgeLog() {
    $this->log = array();
  }

  /**
   * Send or retrieve data from/to Akismet.
   *
   * @param string $method
   *   The HTTP method to use; i.e., 'GET', 'POST', or 'PUT'.
   * @param string $path
   *   The REST path/resource to request; e.g., 'site/1a2b3c'.
   * @param array $data
   *   An associative array of query parameters to send with the request.
   * @param array $expected
   *   (optional) An element that is expected in the response, denoted as a list
   *   of parent element keys to the element and the element key itself; e.g., a
   *   value of array('content', 'id') expects $response['content']['id'] to
   *   exist in the response.
   *
   * @return mixed
   *   On success, the parsed response body. On failure, the last response code,
   *   in case it is a known one; otherwise Akismet::NETWORK_ERROR.
   *
   * @see Akismet::handleRequest()
   * @see Akismet::request()
   */
  public function query($method, $path, array $data = array(), array $expected = array()) {
    // Send the request to the server.
    $server = 'https://' . $this->server;
    $max_attempts = $this->requestMaxAttempts;
    while ($max_attempts-- > 0) {
      try {
        $result = $this->handleRequest($method, $server, $path, $data, $expected);
      }
      catch (AkismetBadRequestException $e) {
        // Irrecoverable error, so don't try further.
        break;
      }
      catch (AkismetAuthenticationException $e) {
        // Irrecoverable error, so don't try further.
        break;
      }
      catch (AkismetException $e) {
        // If the requested resource does not exist, or the request was
        // malformed, there is no point in trying further.
        if ($e->getCode() >= 400 && $e->getCode() < 500) {
          break;
        }
      }
      // Unless we have a positive result, try again.
      if ($this->lastResponseCode === TRUE) {
        break;
      }
    }

    // Write all captured log messages.
    if ($this->writeLog) {
      $this->writeLog();
    }

    // If there is a result and the last request succeeded, return the result to
    // the caller.
    if (isset($result) && $this->lastResponseCode === TRUE) {
      return $result;
    }
    // If the last request succeeded but there was a unexpected response, return
    // the error code.
    if ($this->lastResponseCode === self::RESPONSE_ERROR) {
      return $this->lastResponseCode;
    }
    // Return an authentication error, which may require special client-side
    // processing.
    if ($this->lastResponseCode === self::AUTH_ERROR) {
      return $this->lastResponseCode;
    }
    // Return a request error, which always requires client-side measures to
    // resolve the problem.
    if ($this->lastResponseCode >= self::REQUEST_ERROR && $this->lastResponseCode < 500) {
      return $this->lastResponseCode;
    }

    // In case of any kind of HTTP error (0 [invalid-address],
    // -1002 [bad URI], etc), return a generic NETWORK_ERROR.
    return self::NETWORK_ERROR;
  }

  /**
   * Prepares an HTTP request to the Akismet server and processes the response.
   *
   * @param string $method
   *   The HTTP method to use; i.e., 'GET' or 'POST'.
   * @param string $server
   *   The base URL of the server to perform the request against; e.g.,
   *   'http://rest.akismet.com'.
   * @param string $path
   *   The REST path/resource to request; e.g., 'site/1a2b3c'.
   * @param array $data
   *   An associative array of query parameters to send with the request.
   * @param array $expected
   *   (optional) An element that is expected in the response, denoted as a list
   *   of parent element keys to the element and the element key itself; e.g., a
   *   value of array('content', 'id') expects $response['content']['id'] to
   *   exist in the response. If the expected element does not exist, a
   *   AkismetResponseException is thrown.
   *
   * @return array
   *   An associative array representing the parsed response body on success. On
   *   any failure, a AkismetException is thrown. Additionally,
   *   Akismet::lastResponseCode is set to TRUE on success, or to the Akismet or
   *   HTTP status code on failure.
   *
   * @throws AkismetNetworkException
   * @throws AkismetAuthenticationException
   * @throws AkismetResponseException
   * @throws AkismetException
   *
   * @see Akismet::lastResponseCode
   * @see Akismet::query()
   * @see Akismet::httpBuildQuery()
   * @see Akismet::parseXML()
   * @see json_decode()
   */
  protected function handleRequest($method, $server, $path, $data, $expected = array()) {
    $time_start = microtime(TRUE);
    if (!empty($this->key)) {
      // @todo Move into class property.
      $headers['Accept'] = 'application/xml, application/json;q=0.8, */*;q=0.5';
      if ($method == 'POST') {
        $headers['Content-Type'] = 'application/x-www-form-urlencoded';
      }

      // Append API version to REST endpoint.
      $server .= '/' . self::API_VERSION;
      // Add OAuth request parameters.
      $query = $this->addAuthentication($method, $server, $path, $data, $headers);

      $response = $this->request($method, $server, $path, $query, $headers);

      // Determine basic error condition based on HTTP status code.
      $response->isError = ($response->code < 200 || $response->code >= 300);
    }
    else {
      $headers = array();
      $response = new stdClass;
      $response->code = self::AUTH_ERROR;
      $response->message = 'Missing API keys.';
      $response->isError = TRUE;
      $response->body = NULL;
    }
    $time_stop = microtime(TRUE);

    // Parse the response body string into an array.
    $response->data = array();
    if (isset($response->body) && isset($response->headers['content-type'])) {
      if (strstr($response->headers['content-type'], 'application/json')) {
        $response->data = json_decode($response->body, TRUE);
      }
      elseif (strstr($response->headers['content-type'], 'application/xml')) {
        try {
          $response->elements = new SimpleXmlIterator($response->body);
          $response->data = $this->parseXML($response->elements);
        }
        catch (Exception $e) {
          // Invalid XML so just set the data back to blank.
          $response->data = array();
        }
      }
    }

    // A 'code' in the response has precedence, regardless of a possibly
    // positive HTTP status code.
    if (isset($response->data['code']) && $response->data['code'] != 200) {
      $response->isError = TRUE;
      // Replace HTTP status code with 'code' from response.
      $response->code = (int) $response->data['code'];
      // If there is no HTTP status message, take over 'message' from response,
      // if any.
      if (!isset($response->message) && isset($response->data['message'])) {
        $response->message = $response->data['message'];
      }
    }

    // Verify that the expected (parent) element exists in the response.
    // There is no notion of DTD/schema in JSON. A dedicated class for each
    // response would be a large overhead. Therefore, response validation is
    // limited to checking for one expected element (in a nested array of
    // variable depth).
    if (!$response->isError && !empty($response->data) && !empty($expected)) {
      $ref = &$response->data;
      $parent = reset($expected);
      while ($parent !== FALSE && is_array($ref) && array_key_exists($parent, $ref)) {
        $ref = &$ref[$parent];
        $parent = next($expected);
      }
      // Only if $parent is FALSE we have reached the last expected key.
      if ($parent !== FALSE) {
        $response->isError = TRUE;
        $response->code = self::RESPONSE_ERROR;
        $response->message = 'Unexpected server response.';
      }
    }

    $request_info = array(
      'request' => $method . ' ' . $server . '/' . $path,
      'headers' => $headers,
      'data' => $data,
      'response_code' => $response->code,
      'response_message' => $response->message,
      'response' => !empty($response->data) ? $response->data : $response->body,
      'response_time' => $time_stop - $time_start,
    );
    if ($response->isError) {
      if ($response->code <= 0) {
        throw new AkismetNetworkException('Network error.', self::NETWORK_ERROR, NULL, $this, $request_info);
      }
      if ($response->code == self::AUTH_ERROR) {
        // Check whether authentication failed due to a too large time offset.
        if (isset($response->headers['date'])) {
          // Parse the 'Date' HTTP header in the response into a UNIX timestamp;
          // strtotime() normally performs poorly on date operations, but in
          // this case, there is a standardized date format and timezone
          // conversion, so it is safe to use.
          $offset = abs(time() - strtotime($response->headers['date']));
          // The abs() above turns a negative offset into an absolute integer
          // value, so the difference is always positive.
          if ($offset > self::TIME_OFFSET_MAX) {
            throw new AkismetBadRequestException(sprintf('Invalid client system time: Too large offset from UTC: %s seconds.', $offset), self::REQUEST_ERROR, NULL, $this, $request_info);
          }
        }
        throw new AkismetAuthenticationException('Invalid authentication.', self::AUTH_ERROR, NULL, $this, $request_info);
      }
      if ($response->code == self::RESPONSE_ERROR || $response->code >= 500) {
        throw new AkismetResponseException($response->message, $response->code, NULL, $this, $request_info);
      }
      throw new AkismetException($response->message, $response->code, NULL, $this, $request_info);
    }
    else {
      $this->lastResponseCode = TRUE;
      // No message is logged in case of success.
      $this->log[] = array(
          'severity' => 'debug',
        ) + $request_info;

      return $response->data;
    }
  }

  /**
   * Returns GET/POST request parameters after adding 2-legged OAuth authentication parameters.
   *
   * All available OAuth libraries for PHP are needlessly over-engineered,
   * poorly written and maintained, contain code for server implementations, and
   * would be mostly overhead for the simple purpose of 2-legged authentication.
   * Therefore, this simple function implements OAuth request signing based on
   * latest RFC 5849, using the public API key as client/consumer key and the
   * private API key as client secret.
   *
   * Make sure that your server time is synchronized with the world clocks, and
   * that you do not share your private key with anyone else.
   *
   * @param string $method
   *   The HTTP method to use; i.e., 'GET' or 'POST'.
   * @param string $server
   *   The base URL of the server to perform the request against; e.g.,
   *   'http://foo.akismet.com'.
   * @param string $path
   *   The REST path/resource to request; e.g., 'site/1a2b3c'.
   * @param array $data
   *   An associative array of query parameters to send with the request. Passed
   *   by reference.
   * @param array $headers
   *   An associative array of HTTP request headers to send along with the
   *   request. Passed by reference.
   *
   * @return string
   *   A string containing encoded request parameters derived of $data to use as
   *   GET query string or POST body data.
   *
   * @see http://tools.ietf.org/html/rfc5849
   * @see Akismet::oAuthStrategy
   */
  public function addAuthentication($method, $server, $path, &$data, &$headers) {
    $oauth['oauth_consumer_key'] = $this->publicKey;
    $oauth['oauth_version'] = '1.0';
    // Random string; must be unique across all requests with the same
    // timestamp, client credentials, and token combinations. (3.3)
    $oauth['oauth_nonce'] = md5(microtime() . mt_rand());
    // Number of seconds since January 1, 1970 00:00:00 GMT. (3.3)
    $oauth['oauth_timestamp'] = time();
    $oauth['oauth_signature_method'] = 'HMAC-SHA1';

    // Prepare the request query parameters to return (and pass on to request())
    // and the query parameters to include in the OAuth signature base string.
    // Note that Akismet::httpBuildQuery() sorts parameters already.
    if ($this->oAuthStrategy == 'header') {
      $query = $this->httpBuildQuery($data);
      $oauth_query = $this->httpBuildQuery($oauth + $data);
    }
    elseif ($this->oAuthStrategy == 'query') {
      $data += $oauth;
      $oauth_query = $query = $this->httpBuildQuery($data);
    }
    // Skip authentication entirely; required to create testing site record when
    // running tests.
    else {
      $query = $this->httpBuildQuery($data);
      $oauth_query = '';
    }

    // Generate the signature. (3.4.1)
    // Base string to sign is compound of
    // - uppercase HTTP method
    // - rawurlencoded lowercase server URI (without default ports 80/443),
    //   including path in natural case
    // - encoded, sorted, and lastly (double-)rawurlencoded request parameters,
    //   having GET query parameters first, and POST body data parameters last
    //   (if any)
    // delimited by "&". (3.4.1.1)
    $base_string = implode('&', array(
      $method,
      self::rawurlencode($server . '/' . $path),
      self::rawurlencode($oauth_query),
    ));
    // Key is unconditionally compound of client secret and token secret, even
    // if empty. (3.4.2)
    $key = self::rawurlencode($this->privateKey) . '&' . '';
    $oauth['oauth_signature'] = base64_encode(hash_hmac('sha1', $base_string, $key, TRUE));

    // Add the OAuth protocol parameters as HTTP header, or append the signature
    // to query parameters.
    if ($this->oAuthStrategy == 'header') {
      foreach ($oauth as $key => $value) {
        $oauth[$key] = $key . '="' . self::rawurlencode($value) . '"';
      }
      // Header parameters are joined and delimited by comma. (3.5.1)
      $headers['Authorization'] = 'OAuth ' . implode(', ', $oauth);
    }
    elseif ($this->oAuthStrategy == 'query') {
      $oauth['oauth_signature'] = rawurlencode($oauth['oauth_signature']);
      $data['oauth_signature'] = $oauth['oauth_signature'];
      $query .= '&oauth_signature=' . $oauth['oauth_signature'];
    }

    return $query;
  }

  /**
   * Encodes a URL compliant with OAuth RFC 5849.
   *
   * @param string $value
   *   The URL string to be encoded.
   *
   * @return string
   *   The rawurlencode()d URL, containing string literals for "~" (tildes) and
   *   " " (spaces). (RFC 5849 3.4.1.3.1 and 3.6)
   */
  public static function rawurlencode($value) {
    return strtr(rawurlencode($value), array('%7E' => '~', '+' => ' '));
  }

  /**
   * Performs a HTTP request to a Akismet server.
   *
   * @param string $method
   *   The HTTP method to use; i.e., 'GET', 'POST', or 'PUT'.
   * @param string $server
   *   The base URL of the server to perform the request against; e.g.,
   *   'http://foo.akismet.com'.
   * @param string $path
   *   The REST path/resource to request; e.g., 'site/1a2b3c'.
   * @param string $query
   *   (optional) A prepared string of HTTP query parameters to append to $path
   *   for $method GET, or to use as request body for $method POST.
   * @param array $headers
   *   (optional) An associative array of HTTP request headers to send along
   *   with the request.
   *
   * @return object
   *   An object containing response properties:
   *   - code: The HTTP status code as integer returned by the Akismet server.
   *   - message: The HTTP status message string returned by the Akismet server,
   *     or NULL if there is no message.
   *   - headers: An associative array containing the HTTP response headers
   *     returned by the Akismet server. Header name keys are expected to be
   *     lower-case; i.e., "content-type" instead of "Content-Type".
   *   - body: The HTTP response body string returned by the Akismet server, or
   *     NULL if there is none.
   *
   * @see Akismet::handleRequest()
   */
  abstract protected function request($method, $server, $path, $query = NULL, array $headers = array());

  /**
   * Converts a SimpleXMLIterator structure into an associative array.
   *
   * Used to parse an XML response from Akismet servers into a PHP array. For
   * example:
   * @code
   * $elements = new SimpleXmlIterator($response_body);
   * $parsed_response = $this->parseXML($elements);
   * @endcode
   *
   * @param SimpleXMLIterator $sxi
   *   A SimpleXMLIterator structure of the server response body.
   *
   * @return array
   *   An associative, possibly multidimensional array.
   */
  public static function parseXML(SimpleXMLIterator $sxi) {
    $a = array();
    $remove = array();
    for ($sxi->rewind(); $sxi->valid(); $sxi->next()) {
      $key = $sxi->key();

      // Recurse into non-scalar values.
      if ($sxi->hasChildren()) {
        $value = self::parseXML($sxi->current());
      }
      // Use a simple key/value pair for scalar values.
      else {
        $value = strval($sxi->current());
      }

      if (!isset($a[$key])) {
        $a[$key] = $value;
      }
      // Convert already existing keys into indexed keys, retaining other
      // existing keys in the array; i.e., two or more XML elements of the
      // same name and on the same level.
      // Note that this XML to PHP array conversion does not support multiple
      // different elements that each appear multiple times.
      else {
        // First time we reach here, convert the existing keyed item. Do not
        // remove $key, so we enter this path again.
        if (!isset($remove[$key])) {
          $a[] = $a[$key];
          // Mark $key for removal.
          $remove[$key] = $key;
        }
        // Add the new item.
        $a[] = $value;
      }
    }
    // Lastly, remove named keys that have been converted to indexed keys.
    foreach ($remove as $key) {
      unset($a[$key]);
    }
    return $a;
  }

  /**
   * Builds an RFC-compliant, rawurlencoded query string.
   *
   * PHP did a design decision to only support HTTP query parameters in the form
   * of foo[]=1&foo[]=2, primarily for its built-in and automated conversion to
   * PHP arrays. Other platforms (including the Akismet backend) do not support
   * this syntax and expect multiple parameters to be in the form of
   * foo=1&foo=2.
   *
   * @see http_build_query()
   * @see http://en.wikipedia.org/wiki/Query_string
   * @see http://tools.ietf.org/html/rfc3986#section-3.4
   *
   * @param array $query
   *   The query parameter array to be processed, e.g. $_GET.
   * @param string $parent
   *   Internal use only. Used to build the $query array key for nested items.
   *
   * @return string
   *   A rawurlencoded string which can be used as or appended to the URL query
   *   string.
   *
   * @see Akismet::httpParseQuery()
   */
  public static function httpBuildQuery(array $query, $parent = '') {
    $params = array();

    foreach ($query as $key => $value) {
      // For indexed (unnamed) child array keys, use the same parameter name,
      // leading to param=foo&param=bar instead of param[]=foo&param[]=bar.
      if ($parent && is_int($key)) {
        $key = rawurlencode($parent);
      }
      else {
        $key = ($parent ? $parent . '[' . rawurlencode($key) . ']' : rawurlencode($key));
      }

      // Recurse into children.
      if (is_array($value)) {
        if (!empty($value)) {
          $params[] = self::httpBuildQuery($value, $key);
        }
        // An empty array means that the parameter accepts a list of values, but
        // the list is empty and no value should be assigned, so pass NULL.
        else {
          $params[] = $key . '=';
        }
      }
      // If a query parameter value is NULL, only append its key, followed by
      // "=" (3.4.1.3.2).
      elseif (!isset($value)) {
        $params[] = $key . '=';
      }
      else {
        $params[] = $key . '=' . rawurlencode($value);
      }
    }

    // Parameters are sorted by name, using ascending byte value ordering. If
    // two or more parameters share the same name, they are sorted by their
    // value. (3.4.1.3.2)
    sort($params, SORT_STRING);

    $result = implode('&', $params);
    // Prior to PHP 5.3.0, rawurlencode encoded tildes (~) as per RFC 1738.
    // Percent-encoded octets corresponding to unreserved characters can be
    // decoded at any time. For example, the octet corresponding to the tilde
    // ("~") character is often encoded as "%7E" by older URI processing
    // implementations; the "%7E" can be replaced by "~" without changing its
    // interpretation.
    // @see http://php.net/manual/en/function.rawurlencode.php
    // @see http://tools.ietf.org/html/rfc3986#section-2.3
    $result = str_replace('%7E', '~', $result);
    return $result;
  }

  /**
   * Parses an RFC-compliant, rawurlencoded query string.
   *
   * Akismet clients normally do not need this function, as they do not need to
   * process requests from a server - unless a client attempts to implement
   * client-side unit testing.
   *
   * @param string $query
   *   The query parameter string to process, e.g. $_SERVER['QUERY_STRING']
   *   (GET) or php://input (POST/PUT).
   *
   * @return array
   *   A query parameter array parsed from $query.
   *
   * @see Akismet::httpBuildQuery()
   * @see parse_str()
   */
  public static function httpParseQuery($query) {
    if ($query === '') {
      return array();
    }
    // Explode parameters into arrays to check for duplicate names.
    $params = array();
    $seen = array();
    $duplicate = array();
    foreach (explode('&', $query) as $chunk) {
      $param = explode('=', $chunk, 2);
      if (isset($seen[$param[0]])) {
        $duplicate[$param[0]] = TRUE;
      }
      $seen[$param[0]] = TRUE;
      $params[] = $param;
    }
    // Implode back into a string.
    $query = '';
    foreach ($params as $param) {
      $query .= $param[0];
      if (isset($duplicate[$param[0]])) {
        $query .= '[]';
      }
      if (isset($param[1])) {
        $query .= '=' . $param[1];
      }
      $query .= '&';
    }
    // Parse query string as usual.
    parse_str($query, $result);
    return $result;
  }

  /**
   * Retrieves GET/HEAD or POST/PUT parameters of an inbound HTTP request.
   *
   * @return array
   *   An array containing either GET/HEAD query string parameters or POST/PUT
   *   post body parameters. Parameter parsing accounts for multiple request
   *   parameters in non-PHP format; e.g., 'foo=one&foo=bar'.
   */
  public static function getServerParameters() {
    if ($_SERVER['REQUEST_METHOD'] == 'GET' || $_SERVER['REQUEST_METHOD'] == 'HEAD') {
      $data = self::httpParseQuery($_SERVER['QUERY_STRING']);
    }
    elseif ($_SERVER['REQUEST_METHOD'] == 'POST' || $_SERVER['REQUEST_METHOD'] == 'PUT') {
      $data = self::httpParseQuery(file_get_contents('php://input'));
    }
    return $data;
  }

  /**
   * Retrieves the OAuth Authorization header of an inbound HTTP request.
   *
   * @return array
   *   An array containing all key/value pairs extracted out of the
   *   'Authorization' HTTP header, if any.
   */
  public static function getServerAuthentication() {
    $header = array();
    // PHP as Apache module provides a SAPI function.
    // PHP 5.4+ enables getallheaders() also for FastCGI.
    if (function_exists('getallheaders')) {
      $headers = getallheaders();
      if (isset($headers['Authorization'])) {
        $input = $headers['Authorization'];
      }
    }
    // PHP as CGI with server/.htaccess configuration (e.g., via mod_rewrite)
    // may transfer/forward HTTP request data into server variables.
    elseif (isset($_SERVER['HTTP_AUTHORIZATION'])) {
      $input = $_SERVER['HTTP_AUTHORIZATION'];
    }
    // PHP as CGI may provide HTTP request data as environment variables.
    elseif (isset($_ENV['HTTP_AUTHORIZATION'])) {
      $input = $_ENV['HTTP_AUTHORIZATION'];
    }
    if (isset($input)) {
      preg_match_all('@([^, =]+)="([^"]*)"@', $input, $header);
      if (!empty($header[1]) && !empty($header[2])) {
        $header = array_combine($header[1], $header[2]);
      }
    }
    return $header;
  }

  /**
   * Pings the Akismet API to validate keys and connectivity.
   *
   * @param string $callback
   *   A callback URL to receive the "pong" response from Akismet.
   * @param string publicKey
   *   (Optional) A public key to use.  If not provided, the current class's
   *   publicKey will be used.
   *
   * @return int
   *   The status code returned from the server.
   */
  public function ping($callback, $publicKey = NULL) {
    if (!isset($publicKey)) {
      $publicKey = $this->publicKey;
    }
    $publicKey = rawurlencode($publicKey);
    $result = $this->query('POST', 'site/' . $publicKey . '/ping', array('url' => $callback));
    return $result;
  }

  /**
   * Retrieves a list of sites accessible to this client.
   *
   * Used by Akismet resellers only.
   *
   * @return array
   *   An array containing site resources, as returned by Akismet::getsite().
   */
  public function getSites() {
    $result = $this->query('GET', 'site', array(), array('list'));
    return isset($result['list']) ? $result['list'] : $result;
  }

  /**
   * Retrieves information about a site.
   *
   * @param string $publicKey
   *   (optional) The public Akismet API key of the site to retrieve. Defaults to
   *   the public key of the client.
   *
   * @return mixed
   *   On success, an associative array containing:
   *   - publicKey: The public Akismet API key of the site.
   *   - privateKey: The private Akismet API key of the site.
   *   - url: The URL of the site.
   *   - email: The e-mail address of the primary contact of the site.
   *   - platformName: (optional) The name of the platform running the site
   *     (e.g., "Drupal").
   *   - platformVersion: (optional) The version of the platform running the
   *     site (e.g., "6.20").
   *   - clientName: (optional) The name of the Akismet client plugin used
   *     (e.g., "Akismet").
   *   - clientVersion: (optional) The version of the Akismet client plugin used
   *     (e.g., "6.15").
   *   - expectedLanguages: (optional) An array of language ISO codes, content
   *     is expected to be submitted in on the site.
   *   On failure, the error response code returned by the server.
   */
  public function getSite($publicKey = NULL) {
    if (!isset($publicKey)) {
      $publicKey = $this->publicKey;
    }
    $publicKey = rawurlencode($publicKey);
    $result = $this->query('GET', 'site/' . $publicKey, array(), array('site', 'publicKey'));
    return isset($result['site']) ? $result['site'] : $result;
  }

  /**
   * Creates a new site.
   *
   * @param array $data
   *   An associative array of properties for the new site. At least 'url' and
   *   'email' are required. See Akismet::getSite() for details.
   *
   * @return mixed
   *   On success, the full site information of the created site; see
   *   Akismet::getSite() for details. On failure, the error response code
   *   returned by the server. Or FALSE if 'url' or 'email' was not specified.
   */
  public function createSite(array $data = array()) {
    if (empty($data['url']) || empty($data['email'])) {
      return FALSE;
    }
    $result = $this->query('POST', 'site', $data, array('site', 'publicKey'));
    return isset($result['site']) ? $result['site'] : $result;
  }

  /**
   * Updates a site.
   *
   * Note that most Akismet clients want to use Akismet::verifyKeys() only. This
   * method is primarily used by Akismet resellers, who are provisioning sites
   * and may need to set other site properties.
   *
   * @param array $data
   *   (optional) An associative array of properties to set for the site. See
   *   Akismet::getSite() for details.
   * @param string $publicKey
   *   (optional) The public Akismet API key of the site to update. Defaults to
   *   the public key of the client.
   *
   * @return mixed
   *   On success, the full site information of the created site; see
   *   Akismet::getSite() for details. On failure, the error response code
   *   returned by the server.
   */
  public function updateSite(array $data = array(), $publicKey = NULL) {
    if (!isset($publicKey)) {
      $publicKey = $this->publicKey;
    }
    $publicKey = rawurlencode($publicKey);
    $result = $this->query('POST', 'site/' . $publicKey, $data, array('site', 'publicKey'));
    return isset($result['site']) ? $result['site'] : $result;
  }

  /**
   * Updates a site to verify API keys and send client information.
   *
   * Akismet API keys are validated in all API calls already. This method should
   * be used when the API keys of a Akismet client are configured for a site. It
   * should be invoked at least once for a site, to send client and version
   * information to Akismet in order to aid with Akismet support requests.
   *
   * @return mixed
   *   TRUE on success. On failure, the error response code returned by the
   *   server; either Akismet::REQUEST_ERROR, Akismet::AUTH_ERROR or
   *   Akismet::NETWORK_ERROR.
   */
  public function verifyKeys() {
    $data = $this->getClientInformation();
    $result = $this->updateSite($data);
    // lastResponseCode will either be TRUE, REQUEST_ERROR, AUTH_ERROR, or
    // NETWORK_ERROR.
    return $this->lastResponseCode === TRUE ? TRUE : $this->lastResponseCode;
  }

  /**
   * Deletes a site.
   *
   * @param string $publicKey
   *   The public Akismet API key of the site to delete.
   *
   * @return bool
   *   TRUE on success, FALSE otherwise.
   */
  public function deleteSite($publicKey) {
    $publicKey = rawurlencode($publicKey);
    $result = $this->query('POST', 'site/' . $publicKey . '/delete');
    return $this->lastResponseCode === TRUE;
  }

  /**
   * Checks user-submitted content with Akismet.
   *
   * @param array $data
   *   An associative array containing any of the keys:
   *   - id: The existing content ID of the content, if it or a variant or
   *     revision of it has been checked before.
   *   - postTitle: The title of the content.
   *   - postBody: The body of the content. If the content consists of multiple
   *     fields, concatenate them into one postBody string, separated by " \n"
   *     (space and line-feed).
   *   - authorName: The (real) name of the content author.
   *   - authorUrl: The homepage/website URL of the content author.
   *   - authorMail: The e-mail address of the content author.
   *   - authorIp: The IP address of the content author.
   *   - authorId: The local user ID on the client site of the content author.
   *   - authorOpenid: An indexed array of Open IDs of the content author.
   *   - checks: An indexed array of strings denoting the checks to perform, one
   *     or more of: 'spam', 'quality', 'profanity', 'language', 'sentiment'.
   *     Defaults to 'spam'.
   *   - type: An optional string identifier to request a special content
   *     classification behavior. Possible values are:
   *     - 'user': Enables classification of 'author*' request parameters as
   *       primary content. postTitle and postBody may be left empty without
   *       negative impact on the classification result. Use this for checking
   *       user registration forms. Optionally pass additional user profile text
   *       fields as postBody.
   *   - unsure: Integer denoting whether a "unsure" response should be allowed
   *     (1) for the 'spam' check (which should lead to CAPTCHA) or not (0).
   *     Defaults to 1.
   *   - strictness: A string denoting the strictness of Akismet checks to
   *     perform; one of 'strict', 'normal', or 'relaxed'. Defaults to 'normal'.
   *   - rateLimit: Seconds that must have passed by for the same author to post
   *     again. Defaults to 15.
   *   - honeypot: The value of a client-side honeypot form element, if
   *     non-empty.
   *   - stored: Integer denoting whether the content has been stored (1) on the
   *     client-side or not (0). Use 0 during form validation, 1 after
   *     successful submission. Defaults to 0.
   *   - url: The absolute URL to the stored content.
   *   - contextUrl: An absolute URL to parent/context content of the stored
   *     content; e.g., the URL of the article or forum thread a comment is
   *     posted on (not the parent comment that was replied to).
   *   - contextTitle: The title of the parent/context content of the stored
   *     content; e.g., the title of the article or forum thread a comment is
   *     posted on (not the parent comment that was replied to).
   *   - contextCreated: The creation date of the parent/context content of the
   *     stored content as a unix timestamp in seconds; e.g., the creation date
   *     of the article or forum thread a comment is posted on (not the parent
   *     comment that was replied to).
   *   - trackingImageId: An optional string identifier used to request an
   *     image beacon.  This is used for form behavior analysis.
   *
   * @return mixed
   *   On success, an associative array representing the full content record,
   *   containing the additional keys:
   *   - spam_score: A floating point value with a precision of 2, ranging
   *     between 0.00 and 1.00; whereas 0.00 denotes 100% spam, 0.50 denotes
   *     "unsure", and 1.00 denotes ham. Only returned if 'spam' was passed for
   *     'checks'.
   *   - spam_classification: The final spam classification; one of 'spam',
   *     'unsure', or 'ham'. Only returned if 'spam' was passed for 'checks'.
   *   - profanityScore: A floating point value with a precision of 2, ranging
   *     between 0.00 and 1.00; whereas 0.00 denotes 0% profanity and 1.00
   *     denotes 100% profanity. Only returned if 'profanity' was passed for
   *     'checks'.
   *   - qualityScore: A floating point value with a precision of 2, ranging
   *     between 0.00 and 1.00; whereas 0.00 denotes poor quality and 1.00
   *     high quality. Only returned if 'quality' was passed for 'checks'.
   *   - sentimentScore: A floating point value with a precision of 2, ranging
   *     between 0.00 and 1.00; whereas 0.00 denotes bad sentiment and 1.00
   *     good sentiment. Only returned if 'sentiment' was passed for 'checks'.
   *   - reason: A string denoting the reason for Akismet's classification; e.g.,
   *     - rateLimit: Author was seen on Akismet-protected sites within the given
   *       'rateLimit' time-frame.
   *   On failure, the error response code returned by the server.
   */
  public function checkContent(array $data = array()) {
    $path = 'content';
    if (!empty($data['id'])) {
      // The ID originates from raw form input. Ensure we hit the right endpoint
      // in case a bogus bot fills in even hidden input fields with random
      // strings, by performing a basic syntax validation.
      if (preg_match('@^[a-z0-9-]+$@i', $data['id'])) {
        $path .= '/' . rawurlencode($data['id']);
      }
      unset($data['id']);
    }
    $result = $this->query('POST', $path, $data, array('content', 'id'));

    // parseXML() can only convert multiple sub-elements into an indexed array.
    if (isset($result['content']['languages']) && is_array($result['content']['languages'])) {
      $result['content']['languages'] = array_values($result['content']['languages']);
    }

    return isset($result['content']) ? $result['content'] : $result;
  }

  /**
   * Retrieves a CAPTCHA resource from Akismet.
   *
   * @param array $data
   *   An associative array containing:
   *   - type: A string denoting the type of CAPTCHA to create; one of 'image'
   *     or 'audio'.
   *   and any of the keys:
   *   - content_id: The ID of a content resource to link the CAPTCHA to. Allows
   *     Akismet to learn when it was unsure.
   *   - ssl: An integer denoting whether to create a CAPTCHA URL using HTTPS
   *     (1) or not (0). Only available for paid subscriptions.
   *
   * @return mixed
   *   On success, an associative array representing the full CAPTCHA record,
   *   containing:
   *   - id: The ID of the CAPTCHA.
   *   - url: The URL of the CAPTCHA.
   *   On failure, the error response code returned by the server.
   *   Or FALSE if a unknown 'type' was specified.
   */
  public function createCaptcha(array $data = array()) {
    if (!isset($data['type']) || !in_array($data['type'], array('image', 'audio'))) {
      return FALSE;
    }
    $path = 'captcha';
    $result = $this->query('POST', $path, $data, array('captcha', 'id'));

    return isset($result['captcha']) ? $result['captcha'] : $result;
  }

  /**
   * Checks whether a user-submitted solution for a CAPTCHA is correct.
   *
   * @param array $data
   *   An associative array containing:
   *   - id: The ID of the CAPTCHA to check.
   *   - solution: The answer provided by the author.
   *   and any of the keys:
   *   - authorName: The (real) name of the content author.
   *   - authorUrl: The homepage/website URL of the content author.
   *   - authorMail: The e-mail address of the content author.
   *   - authorIp: The IP address of the content author.
   *   - authorId: The local user ID on the client site of the content author.
   *   - authorOpenid: An indexed array of Open IDs of the content author.
   *   - rateLimit: Seconds that must have passed by for the same author to post
   *     again. Defaults to 15.
   *   - honeypot: The value of a client-side honeypot form element, if
   *     non-empty.
   *
   * @return mixed
   *   On success, an associative array representing the full CAPTCHA record,
   *   additionally containing:
   *   - solved: Whether the provided solution was correct (1) or not (0).
   *   - reason: A string denoting the reason for Akismet's classification; e.g.,
   *     - rateLimit: Author was seen on Akismet-protected sites within the given
   *       'rateLimit' time-frame.
   *   On failure, the error response code returned by the server.
   *   Or FALSE if no 'id' was specified.
   */
  public function checkCaptcha(array $data = array()) {
    if (empty($data['id'])) {
      return FALSE;
    }
    // The ID originates from raw form input. Ensure we hit the right endpoint
    // in case a bogus bot fills in even hidden input fields with random
    // strings, by performing a basic syntax validation.
    if (!preg_match('@^[a-z0-9-]+$@i', $data['id'])) {
      return FALSE;
    }
    $path = 'captcha/' . rawurlencode($data['id']);
    unset($data['id']);
    $result = $this->query('POST', $path, $data, array('captcha', 'id'));

    return isset($result['captcha']) ? $result['captcha'] : $result;
  }

  /**
   * Sends feedback to Akismet.
   *
   * @param array $data
   *   An associative array containing:
   *   - reason: A string denoting the reason for why the content associated
   *     with either content_id or captcha_id is being reported; one of:
   *     - spam: The content is spam, unsolicited advertising.
   *     - profanity: The content contains obscene, violent, profane language.
   *     - quality: The content is of low quality.
   *     - unwanted: The content is unwanted, taunting, off-topic.
   *  - type (optional): A string denoting the type of feedback submitted.
   *     - moderate: Feedback from the admin moderation process (default).
   *     - flag: feedback from end users flagging content as inappropriate
   *   - source (optional): A string denoting the user-interface source of the feedback.
   *   and at least one of:
   *   - content_id: A Akismet content ID associated with the content.
   *   - captcha_id: A Akismet CAPTCHA ID associated with the content.
   *
   * @return bool
   *   TRUE if the feedback was sent successfully, FALSE otherwise.
   */
  public function sendFeedback(array $data) {
    if (empty($data['content_id']) && empty($data['captcha_id'])) {
      return FALSE;
    }
    if (empty($data['reason'])) {
      return FALSE;
    }
    $this->query('POST', 'feedback', $data);
    return $this->lastResponseCode === TRUE ? TRUE : FALSE;
  }

  /**
   * Retrieves the blacklist for a site.
   *
   * @param string $publicKey
   *   (optional) The public Akismet API key of the site to retrieve the
   *   blacklist for. Defaults to the public key of the client.
   *
   * @return mixed
   *   An array containing blacklist entries; see Akismet::getBlacklistEntry()
   *   for details. On failure, the error response code returned by the server.
   *
   * @todo List parameters.
   */
  public function getBlacklist($publicKey = NULL) {
    if (!isset($publicKey)) {
      $publicKey = $this->publicKey;
    }
    $publicKey = rawurlencode($publicKey);
    $result = $this->query('GET', 'blacklist/' . $publicKey, array(), array('list'));
    return isset($result['list']) ? $result['list'] : $result;
  }

  /**
   * Retrieves a blacklist entry stored for a site.
   *
   * @param string $entryId
   *   The ID of the blacklist entry to retrieve.
   * @param string $publicKey
   *   (optional) The public Akismet API key of the site to retrieve the
   *   blacklist entry for. Defaults to the public key of the client.
   *
   * @return mixed
   *   On success, an associative array containing:
   *   - id: The ID the of blacklist entry.
   *   - created: A timestamp in seconds since the UNIX epoch of when the entry
   *     was created.
   *   - value: The blacklisted string/value.
   *   - reason: A string denoting the reason for why the value is blacklisted;
   *     one of 'spam', 'profanity', 'quality', or 'unwanted'. Defaults to
   *     'unwanted'.
   *   - context: A string denoting where the entry's value may match; one of
   *     'allFields', 'links', 'authorName', 'authorMail', 'authorIp',
   *     'authorIp', or 'postTitle'. Defaults to 'allFields'.
   *   - match: A string denoting how precise the entry's value may match; one
   *     of 'exact' or 'contains'. Defaults to 'contains'.
   *   - status: An integer denoting whether the entry is enabled (1) or not
   *     (0).
   *   - note: A custom string explaining the entry. Useful in a multi-moderator
   *     scenario.
   *   On failure, the error response code returned by the server.
   */
  public function getBlacklistEntry($entryId, $publicKey = NULL) {
    if (!isset($publicKey)) {
      $publicKey = $this->publicKey;
    }
    $path = 'blacklist/' . rawurlencode($publicKey) . '/' . rawurlencode($entryId);
    $result = $this->query('GET', $path, array(), array('entry', 'id'));
    return isset($result['entry']) ? $result['entry'] : $result;
  }

  /**
   * Creates or updates a blacklist entry for a site.
   *
   * @param array $data
   *   An associative array describing the blacklist entry to create or update.
   *   See return value of Akismet::getBlacklistEntry() for details. To update
   *   an existing entry, its ID must be specified in 'id'.
   * @param string $publicKey
   *   (optional) The public Akismet API key of the site to save the blacklist
   *   entry for. Defaults to the public key of the client.
   *
   * @return mixed
   *   On success, the full blacklist entry record of the saved entry; see
   *   Akismet::getBlacklistEntry() for details. On failure, the error response
   *   code returned by the server.
   */
  public function saveBlacklistEntry(array $data = array(), $publicKey = NULL) {
    if (!isset($publicKey)) {
      $publicKey = $this->publicKey;
    }
    $path = 'blacklist/' . rawurlencode($publicKey);
    if (!empty($data['id'])) {
      $path .= '/' . rawurlencode($data['id']);
      unset($data['id']);
    }
    $result = $this->query('POST', $path, $data, array('entry', 'id'));
    return isset($result['entry']) ? $result['entry'] : $result;
  }

  /**
   * Deletes a blacklist entry from a site.
   *
   * @param string $entryId
   *   The ID of the blacklist entry to delete.
   * @param string $publicKey
   *   (optional) The public Akismet API key of the site to create the blacklist
   *   entry for. Defaults to the public key of the client.
   *
   * @return bool
   *   TRUE on success, FALSE otherwise.
   */
  public function deleteBlacklistEntry($entryId, $publicKey = NULL) {
    if (!isset($publicKey)) {
      $publicKey = $this->publicKey;
    }
    $path = 'blacklist/' . rawurlencode($publicKey) . '/' . rawurlencode($entryId) . '/delete';
    $result = $this->query('POST', $path);
    return $this->lastResponseCode === TRUE;
  }

  /**
   * Generates a URL to a tracking image.
   *
   * This image can be used by form behavior analysis to analyze the
   * humanity of the content author.
   *
   * @return array
   *   An associative array of tracking information generated.
   *   - tracking_url: the url to the tracking image (without any http protocol)
   *   - tracking_id: the tracking id generated
   */
  public function getTrackingImage() {
    $public_key = $this->publicKey;
    $ts = time();
    $tracking_id = hash_hmac('sha1', $public_key . $ts, $this->privateKey);

    $result = array(
      'tracking_url' => '//' . $this->server . '/v1/tracking/' . urlencode($tracking_id) . '.png?public_key=' . $public_key . "&timestamp=" . $ts,
      'tracking_id' => $tracking_id,
    );
    return $result;
  }
}

/**
 * A catchable Akismet exception.
 *
 * The Akismet class internally uses exceptions to handle HTTP request errors
 * within the Akismet::handleRequest() method. All exceptions thrown in the
 * Akismet class and derived classes should be instances of the AkismetException
 * class if they pertain to errors that can be caught/handled within the class.
 * Other errors should not use the AkismetException class and handled
 * differently.
 *
 * No AkismetException is supposed to pile up as a user-facing fatal error. All
 * functions that invoke Akismet::handleRequest() have to catch Akismet
 * exceptions.
 *
 * @see Akismet::query()
 * @see Akismet::handleRequest()
 *
 * @param $message
 *   The Exception message to throw.
 * @param $code
 *   The Exception code.
 * @param $previous
 *   (optional) The previous Exception, if any.
 * @param $instance
 *   The Akismet class instance the Exception is thrown in.
 * @param $arguments
 *   (optional) A associative array containing information about a performed
 *   HTTP request that failed:
 *   - request: (string) The HTTP method and URI of the performed request; e.g.,
 *     "GET http://server.akismet.com/v1/foo/bar". In case of GET requests, do
 *     not add query parameters to the URI; pass them in 'data' instead.
 *   - data: (array) An associative array containing HTTP GET/POST/PUT request
 *     query parameters that were sent to the server.
 *   - response: (mixed) The server response, either as string, or the already
 *     parsed response; i.e., an array.
 */
class AkismetException extends Exception {
  /**
   * @var Akismet
   */
  protected $akismet;

  /**
   * The severity of this exception.
   *
   * By default, all exceptions should be logged and appear as errors (unless
   * overridden by a later log entry).
   *
   * @var string
   */
  protected $severity = 'error';

  /**
   * Overrides Exception::__construct().
   */
  function __construct($message = '', $code = 0, Exception $previous = NULL, Akismet $akismet, array $request_info = array()) {
    // Fatal error on PHP <5.3 when passing more arguments to Exception.
    if (version_compare(phpversion(), '5.3') >= 0) {
      parent::__construct($message, $code, $previous);
    }
    else {
      parent::__construct($message, $code);
    }
    $this->akismet = $akismet;

    // Set the error code on the Akismet class.
    $akismet->lastResponseCode = $code;

    // Log the exception.
    $message = array(
      'severity' => $this->severity,
      'message' => 'Error @code: %message (@server-ip)',
      'arguments' => array(
        '@code' => $code,
        '%message' => $message,
      ),
    );
    // Add HTTP request information, if available.
    if (!empty($request_info)) {
      $message += $request_info;
    }
    $akismet->log[] = $message;
  }
}

/**
 * Akismet network error exception.
 *
 * Thrown in case a HTTP request results in code <= 0, denoting a low-level
 * communication error.
 */
class AkismetNetworkException extends AkismetException {
  /**
   * Overrides AkismetException::$severity.
   *
   * The client may be able to recover from this error, so use a warning level.
   */
  protected $severity = 'warning';
}

/**
 * Akismet authentication error exception.
 *
 * Thrown in case API keys or other authentication parameters are invalid.
 */
class AkismetAuthenticationException extends AkismetException {
}

/**
 * Akismet error due to bad client request exception.
 *
 * Thrown in case the local time diverges too much from UTC.
 *
 * @see Akismet::TIME_OFFSET_MAX
 * @see Akismet::REQUEST_ERROR
 * @see Akismet::handleRequest()
 */
class AkismetBadRequestException extends AkismetException {
}

/**
 * Akismet server response exception.
 *
 * Thrown when a request to the Akismet server succeeds, but the response does
 * not contain an expected element; e.g., a backend configuration or execution
 * error.
 *
 * @see Akismet::handleRequest()
 */
class AkismetResponseException extends AkismetException {
  /**
   * Overrides AkismetException::$severity.
   *
   * Might be a client-side error, but more likely a server-side error. The
   * client may be able to recover from this error.
   */
  protected $severity = 'debug';
}
