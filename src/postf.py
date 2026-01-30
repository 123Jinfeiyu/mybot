import argparse
import urllib.request
import urllib.error
import sys
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(
        description='发送POST请求，从文件读取请求体（默认Content-Type: text/plain）'
    )
    
    parser.add_argument('-u', '--url', required=True, help='目标URL')
    parser.add_argument('-f', '--file', required=True, help='包含请求体的文件路径')
    parser.add_argument('--content-type', default='text/plain',
                       help='Content-Type，默认为text/plain')
    
    args = parser.parse_args()
    
    try:
        # 读取文件
        file_path = Path(args.file)
        if not file_path.exists():
            print(f"错误：文件 '{args.file}' 不存在", file=sys.stderr)
            sys.exit(1)
        
        with open(file_path, 'r', encoding='utf-8') as f:
            body_text = f.read()
        
        # 准备请求
        body_data = body_text.encode('utf-8')
        headers = {
            'Content-Type': args.content_type,
            'Content-Length': str(len(body_data))
        }
        
        print(f"发送POST请求到: {args.url}")
        print(f"Content-Type: {args.content_type}")
        print(f"Body内容: {body_text[:100]}{'...' if len(body_text) > 100 else ''}")
        
        # 发送请求
        req = urllib.request.Request(
            args.url,
            data=body_data,
            headers=headers,
            method='POST'
        )
        
        with urllib.request.urlopen(req) as response:
            status = response.getcode()
            response_body = response.read().decode('utf-8')
            
            print(f"\n响应状态码: {status}")
            print("响应内容:")
            print(response_body)
            
    except FileNotFoundError:
        print(f"错误：找不到文件 '{args.file}'", file=sys.stderr)
        sys.exit(1)
    except urllib.error.HTTPError as e:
        print(f"HTTP错误: {e.code} {e.reason}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"错误: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()