/*
 */
CREATE TABLE UART (name String, path String, baud String);
INSERT INTO UART (name, path, baud) VALUES ('uart-0', '/dev/ttyS0', '1000000');
CREATE TABLE SCSCL (
  name String, UART String, id String, max String, min String, offset String
);
INSERT INTO SCSCL (name, UART, id, max, min, offset)
  VALUES ('scs1', '$uart-0', '1', '1023', '0', '0');
