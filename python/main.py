import argparse
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


def main():
    parser = argparse.ArgumentParser(
            description='进行机器人动作的DRL训练。')
    parser.add_argument('file', help='目标.bot文件')
    args = parser.parse_args()

    actor = Actor()
    actor.parse(args.file)
    actor.serialize()

if __name__ == "__main__":
    main()
