/* 负向：INSERT 的表名拼错（Timers）——原实现静默跳过整条 INSERT，
   硬件实例无声消失，错误延迟到动作期以误导信息暴露 */
CREATE TABLE Timer (name String);
INSERT INTO Timers (name) VALUES ('timer-0');
