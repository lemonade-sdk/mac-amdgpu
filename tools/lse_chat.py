#!/usr/bin/env python3
"""Interactive streaming terminal client for the local LSE chat server."""
import argparse
import json
import os
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8080')
    parser.add_argument('--model', default='local-qwen')
    parser.add_argument('--max-tokens', type=int, default=256)
    parser.add_argument('--temperature', type=float, default=0.6)
    args = parser.parse_args()
    if args.max_tokens < 1:
        parser.error('--max-tokens must be positive')
    headers = {'Content-Type': 'application/json'}
    if os.environ.get('LSE_API_KEY'):
        headers['Authorization'] = 'Bearer ' + os.environ['LSE_API_KEY']
    messages = []
    print('LSE · Qwen3.8-27B · streaming chat')
    print('/clear starts a new conversation; /quit exits.\n')
    while True:
        try:
            prompt = input('You > ').strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if prompt == '/quit':
            break
        if prompt == '/clear':
            messages.clear()
            print('Conversation cleared.\n')
            continue
        if not prompt:
            continue
        pending = messages + [{'role': 'user', 'content': prompt}]
        payload = {'model': args.model, 'messages': pending, 'stream': True,
                   'max_tokens': args.max_tokens, 'temperature': args.temperature}
        request = urllib.request.Request(
            args.url.rstrip('/') + '/v1/chat/completions',
            data=json.dumps(payload).encode(), headers=headers)
        parts, timings, reason, completed = [], {}, None, False
        print('Qwen > ', end='', flush=True)
        try:
            with urllib.request.urlopen(request, timeout=600) as response:
                for line in response:
                    if not line.startswith(b'data:'):
                        continue
                    data = line[5:].strip()
                    if data == b'[DONE]':
                        completed = True
                        break
                    item = json.loads(data)
                    if item.get('error'):
                        raise ValueError(str(item['error']))
                    timings = item.get('timings', timings)
                    for choice in item.get('choices', []):
                        text = choice.get('delta', {}).get('content', '') or ''
                        print(text, end='', flush=True)
                        parts.append(text)
                        reason = choice.get('finish_reason') or reason
            if not completed:
                raise ValueError('stream ended before completion')
        except KeyboardInterrupt:
            print('\nChat client closed. The server may still be finishing this request.')
            return
        except (urllib.error.URLError, OSError, ValueError) as error:
            detail = error.read().decode(errors='replace') if isinstance(error, urllib.error.HTTPError) else str(error)
            print('\nRequest failed: ' + detail)
            print('Ensure the server is listening; use /clear if the conversation exceeds its context.\n')
            continue
        messages = pending + [{'role': 'assistant', 'content': ''.join(parts)}]
        print()
        if timings:
            print(f"[PP/s {timings.get('prompt_per_second', 0):.2f} · "
                  f"TPS {timings.get('decode_per_second', 0):.2f}]")
        if reason == 'length':
            print('[Response reached the token limit.]')
        print()


if __name__ == '__main__':
    main()
