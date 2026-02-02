import argparse
import random
import torch
import torch.nn as nn
import os

class NeuralNetwork(nn.Module):
    def __init__(self):
        super(NeuralNetwork, self).__init__()
        # 定义层
        self.fc1 = nn.Linear(3060, 5200)
        self.relu = nn.ReLU()
        self.fc2 = nn.Linear(5200, 3060)
        self.sigmoid = nn.Sigmoid()
        self.dropout = nn.Dropout(0.5)

    def forward(self, x):
        # 定义前向传播
        x = self.fc1(x)
        x = self.relu(x)
        x = self.dropout(x)
        x = self.fc2(x)
        x = self.sigmoid(x)
        return x
    
    def save_model(self, path):
        """保存模型权重"""
        torch.save(self.state_dict(), path)
        print(f"Model saved to {path}")
    
    def load_model(self, path):
        """加载模型权重"""
        self.load_state_dict(torch.load(path))
        print(f"Model loaded from {path}")
        return self

class Actor:
    '''硬件机器人类'''

    def __init__(self):
        self.ls = []
        self.at = 0
    def parse(self, filepath: str):
        '''解析脚步'''
        script = self.open_file(filepath)
        self.nl = True  # new line
        self.sl = False # special
        self.cm = False # comment
        self.tmp = ''
        self.tmpls=[]
        self.ls=[]
        self.at = 0
        for c in script:
            self.next(c)

    @staticmethod
    def open_file(path: str):
        with open(path, 'r') as f:
            return f.read()

    def next(self, c: str):
        if not self.cm and c == '#':
            self.cm = True
        elif self.cm and c == '\n':
            self.nl = True
            self.sl = False
            self.cm = False
        elif self.cm and c != '\n':
            ...
        elif c == '\n':
            ...
        elif self.nl and c == '!':
            self.nl = False
            self.sl = True
        elif c == ' ':
            if self.tmp != '':
                a = float(self.tmp)
                self.tmpls.append(a)
                self.tmp = ''
        elif c == ';':
            if self.tmp != '':
                a = float(self.tmp)
                self.tmpls.append(a)
                self.tmp = ''
            if self.tmpls != []:
                self.ls.append(self.tmpls)
                self.tmpls = []
                if self.sl:
                    self.at += 1
                self.nl = True
                self.sl = False
                self.cm = False
        else:
            self.tmp += c

    def serialize(self) -> str:
        msg = ''
        for i in range(len(self.ls)):
            nl = True
            if i < self.at:
                msg += '!'
            for j in self.ls[i]:

                if nl:
                    nl = False
                else:
                    msg += ' '

                msg += str(j)

            msg += ';\n'
        return msg

    def nn_prefix(self, model_path: str):
        model = NeuralNetwork()
        if os.path.exists(model_path):
            model.load_model(model_path)    
        criterion = nn.MSELoss()
        optimizer = torch.optim.Adam(model.parameters(), lr=0.001)
        
        # 用于存储梯度的列表
        gradients = []

        # 奖励列表
        rewards = [1,1 ,2, -1, -1, -1]

        for i in range(len(self.ls)-self.at):
            i = i + self.at
            X = torch.zeros((1,  3060))
            ii =self.ls[i]
            for j in range(len(ii)):
                jj  =int(ii[j])
                X[0, j * 181 + jj] = torch.tensor(1) # one-hot
        
            # 前向传播
            output = model(X)
            
            # DRL Q - 使用out-of-place操作避免inplace错误
            # 创建一个新的张量来存储修改后的值
            modified_output = torch.zeros_like(output)
            for j in range(output.shape[1]):
                val = output[0, j].item() + random.uniform(-0.1, 0.1)
                val = max(0, min(1, val))
                modified_output[0, j] = val
            
            print(len(self.ls), i)
            # 输出切片（使用modified_output，但保持原始output用于损失计算）
            for j in range(16):
                jj = int(modified_output[0, j * 181 : (j + 1) * 181].argmax())
                self.ls[i][j] = jj
            
            # 计算DRL误差（使用原始output计算损失，保持梯度连接）
            target = torch.zeros_like(output)
            for j in range(16):
                jj = int(modified_output[0, j * 181 : (j + 1) * 181].argmax())
                target[0, j * 181 + jj] = 1.0 # one-hot，使用标量而不是张量

            # 确保target不需要梯度
            target = target.detach()
            loss = criterion(output, target)
            print(f"Step {i}, Loss: {loss.item()}")

            # 计算奖励（使用负损失作为奖励）
            ...

            # 反向传播
            optimizer.zero_grad()
            loss.backward()
            #optimizer.step() # 延迟 


            # 储存梯度
            grads = {}
            for name, param in model.named_parameters():
                if param.grad is not None:
                    grads[name] = param.grad.clone()
            gradients.append(grads)
            
            # 清空梯度，避免累积
            optimizer.zero_grad() 
        
        # 模拟奖励
        rewards = [0, 1, -10]
        for ii in range(len(rewards)):
            reward = rewards[ii]

            # 折扣系数
            discount_factor = 0.98
            # 奖励阈值
            threshold = 0.01

            for jj in range(ii).__reversed__(): 
                reward *= discount_factor
                if abs(reward) < threshold:
                    break   
                    
                # 计算奖励的标准化值（可选）
                if len(rewards) > 0:
                    reward_mean = sum(rewards) / len(rewards)
                    reward_std = max(1e-8, (sum((r - reward_mean)**2 for r in rewards) / len(rewards))**0.5)
                    normalized_rewards = [(r - reward_mean) / reward_std for r in rewards]
                else:
                    normalized_rewards = [1.0] * len(gradients)
                
                # 累加梯度（根据奖励加权）
                for name, param in model.named_parameters():
                    if param.requires_grad:
                        param.grad = torch.zeros_like(param.data)
                        for grad_idx, grad_dict in enumerate(gradients):
                            if name in grad_dict:
                                # 根据奖励加权梯度
                                weight = normalized_rewards[grad_idx] if grad_idx < len(normalized_rewards) else 1.0
                                param.grad += grad_dict[name] * weight
                # 取平均
                param.grad /= len(gradients)
                
                # 执行优化步骤
                optimizer.step()
                # 清空梯度
                optimizer.zero_grad()

                
        model.save_model(model_path)

def main():
    parser = argparse.ArgumentParser(
            description='进行机器人动作的DRL训练。')
    parser.add_argument('file', help='目标.bot文件')
    args = parser.parse_args()

    actor = Actor()
    actor.parse(args.file)
    actor.nn_prefix("model.pth")
    print(actor.serialize())

if __name__ == "__main__":
    main()
